// Unit tests for platform_profile — Pi model detection, per-model
// hardware profiles, PulseAudio sink resolution, and gpiochip selection.
// Pure logic, no Pi hardware required; runs on the dev machine.

#include <catch2/catch_test_macros.hpp>

#include "platform/platform_profile.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace platform;

// ---------------------------------------------------------------
// parse_pi_model
// ---------------------------------------------------------------

TEST_CASE("parse_pi_model recognizes Pi 4B model string") {
    REQUIRE(parse_pi_model("Raspberry Pi 4 Model B Rev 1.4") == PiModel::Pi4);
}

TEST_CASE("parse_pi_model recognizes Pi 5 model string") {
    REQUIRE(parse_pi_model("Raspberry Pi 5 Model B Rev 1.0") == PiModel::Pi5);
}

TEST_CASE("parse_pi_model handles trailing NUL from device-tree") {
    // /proc/device-tree/model is NUL-terminated; readers that slurp the
    // file get the NUL embedded in the std::string.
    std::string dt_model("Raspberry Pi 5 Model B Rev 1.0\0", 31);
    REQUIRE(parse_pi_model(dt_model) == PiModel::Pi5);
}

TEST_CASE("parse_pi_model returns Unknown for non-Pi hardware") {
    REQUIRE(parse_pi_model("") == PiModel::Unknown);
    REQUIRE(parse_pi_model("Generic x86_64 PC") == PiModel::Unknown);
    // Pi 3 is not a supported MDB target — must not match Pi4/Pi5.
    REQUIRE(parse_pi_model("Raspberry Pi 3 Model B Plus Rev 1.3") == PiModel::Unknown);
}

// ---------------------------------------------------------------
// profile_for
// ---------------------------------------------------------------

TEST_CASE("Pi4 profile has analog audio and bcm2711 gpio label") {
    PlatformProfile p = profile_for(PiModel::Pi4);
    REQUIRE(p.model == PiModel::Pi4);
    REQUIRE(p.has_analog_audio);
    REQUIRE(std::find(p.gpiochip_labels.begin(), p.gpiochip_labels.end(),
                      "pinctrl-bcm2711") != p.gpiochip_labels.end());
}

TEST_CASE("Pi5 profile has no analog audio and rp1 gpio label first") {
    PlatformProfile p = profile_for(PiModel::Pi5);
    REQUIRE(p.model == PiModel::Pi5);
    REQUIRE_FALSE(p.has_analog_audio);
    REQUIRE_FALSE(p.gpiochip_labels.empty());
    REQUIRE(p.gpiochip_labels.front() == "pinctrl-rp1");
}

TEST_CASE("Unknown profile is conservative: no analog audio, tries all known labels") {
    PlatformProfile p = profile_for(PiModel::Unknown);
    REQUIRE_FALSE(p.has_analog_audio);
    // Must be able to find the header chip on any supported board.
    REQUIRE(std::find(p.gpiochip_labels.begin(), p.gpiochip_labels.end(),
                      "pinctrl-rp1") != p.gpiochip_labels.end());
    REQUIRE(std::find(p.gpiochip_labels.begin(), p.gpiochip_labels.end(),
                      "pinctrl-bcm2711") != p.gpiochip_labels.end());
}

// ---------------------------------------------------------------
// detect_platform (file-based)
// ---------------------------------------------------------------

// Only used by the two device-tree-file tests below, which are themselves
// #ifndef __APPLE__ (detect_platform() short-circuits to PiModel::Mac under
// __APPLE__, so this helper would otherwise be unused there).
#ifndef __APPLE__
namespace {
std::string write_temp_model_file(const std::string& contents) {
    auto path = std::filesystem::temp_directory_path() /
                "mdb_test_dt_model.txt";
    std::ofstream f(path, std::ios::binary);
    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path.string();
}
} // namespace
#endif

// These two assume device-tree-file-based detection, which only applies
// off Apple hosts: detect_platform() short-circuits to PiModel::Mac under
// __APPLE__ regardless of model_path (see the Mac host TEST_CASEs below).
#ifndef __APPLE__
TEST_CASE("detect_platform reads a device-tree model file") {
    std::string dt("Raspberry Pi 5 Model B Rev 1.0\0", 31);
    auto path = write_temp_model_file(dt);
    PlatformProfile p = detect_platform(path);
    std::remove(path.c_str());
    REQUIRE(p.model == PiModel::Pi5);
    REQUIRE_FALSE(p.has_analog_audio);
}

TEST_CASE("detect_platform returns Unknown profile when file is missing") {
    PlatformProfile p = detect_platform("/nonexistent/dt/model");
    REQUIRE(p.model == PiModel::Unknown);
}
#endif

// ---------------------------------------------------------------
// PulseAudio sink resolution
// ---------------------------------------------------------------

// Real-world `pactl list short sinks` shapes:
static const char* kPi4Sinks =
    "0\talsa_output.platform-fe00b840.mailbox.stereo-fallback\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tSUSPENDED\n"
    "1\talsa_output.platform-fef00700.hdmi.hdmi-stereo\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tRUNNING\n";

static const char* kPi5Sinks =
    "0\talsa_output.platform-107c701400.hdmi.hdmi-stereo\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tIDLE\n";

static const char* kPi5SinksWithUsbDac =
    "0\talsa_output.platform-107c701400.hdmi.hdmi-stereo\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tIDLE\n"
    "1\talsa_output.usb-C-Media_Electronics_Inc._USB_Audio_Device-00.analog-stereo\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tSUSPENDED\n";

TEST_CASE("find_hdmi_sink picks the HDMI sink on Pi 4") {
    auto s = find_hdmi_sink(kPi4Sinks);
    REQUIRE(s.has_value());
    REQUIRE(*s == "alsa_output.platform-fef00700.hdmi.hdmi-stereo");
}

TEST_CASE("find_hdmi_sink picks the HDMI sink on Pi 5 despite different platform address") {
    auto s = find_hdmi_sink(kPi5Sinks);
    REQUIRE(s.has_value());
    REQUIRE(*s == "alsa_output.platform-107c701400.hdmi.hdmi-stereo");
}

TEST_CASE("find_analog_sink picks the 3.5mm mailbox sink on Pi 4") {
    auto s = find_analog_sink(kPi4Sinks);
    REQUIRE(s.has_value());
    REQUIRE(*s == "alsa_output.platform-fe00b840.mailbox.stereo-fallback");
}

TEST_CASE("find_analog_sink picks a USB DAC analog sink but never an HDMI sink") {
    auto s = find_analog_sink(kPi5SinksWithUsbDac);
    REQUIRE(s.has_value());
    REQUIRE(*s == "alsa_output.usb-C-Media_Electronics_Inc._USB_Audio_Device-00.analog-stereo");

    REQUIRE_FALSE(find_analog_sink(kPi5Sinks).has_value());
}

TEST_CASE("resolve_sink falls back to HDMI when analog requested but absent") {
    auto s = resolve_sink(kPi5Sinks, SinkChoice::Analog);
    REQUIRE(s.has_value());
    REQUIRE(*s == "alsa_output.platform-107c701400.hdmi.hdmi-stereo");
}

TEST_CASE("resolve_sink honors the requested kind when it exists") {
    auto hdmi = resolve_sink(kPi4Sinks, SinkChoice::Hdmi);
    REQUIRE(hdmi.has_value());
    REQUIRE(*hdmi == "alsa_output.platform-fef00700.hdmi.hdmi-stereo");

    auto analog = resolve_sink(kPi4Sinks, SinkChoice::Analog);
    REQUIRE(analog.has_value());
    REQUIRE(*analog == "alsa_output.platform-fe00b840.mailbox.stereo-fallback");
}

TEST_CASE("resolve_sink returns nothing when no sinks exist") {
    REQUIRE_FALSE(resolve_sink("", SinkChoice::Hdmi).has_value());
    REQUIRE_FALSE(resolve_sink("", SinkChoice::Analog).has_value());
}

// ---------------------------------------------------------------
// gpiochip selection by label
// ---------------------------------------------------------------

TEST_CASE("pick_gpiochip finds the Pi 4 header chip by label") {
    // Pi 4: single chip, index 0.
    std::vector<std::string> chips = {"pinctrl-bcm2711"};
    REQUIRE(pick_gpiochip(chips, profile_for(PiModel::Pi4).gpiochip_labels) == 0);
}

TEST_CASE("pick_gpiochip finds the RP1 header chip even when not chip 0") {
    // Pi 5 at launch firmware: gpiochip0-3 are SoC-internal, header is chip 4.
    std::vector<std::string> chips = {
        "gpio-brcmstb@107d508500", "gpio-brcmstb@107d508520",
        "gpio-brcmstb@107d517c00", "gpio-brcmstb@107d517c20",
        "pinctrl-rp1"};
    REQUIRE(pick_gpiochip(chips, profile_for(PiModel::Pi5).gpiochip_labels) == 4);
}

TEST_CASE("pick_gpiochip finds RP1 at index 0 on current Pi 5 kernels") {
    // Kernel 6.6.47+ renumbered the header chip back to gpiochip0.
    std::vector<std::string> chips = {
        "pinctrl-rp1",
        "gpio-brcmstb@107d508500", "gpio-brcmstb@107d508520"};
    REQUIRE(pick_gpiochip(chips, profile_for(PiModel::Pi5).gpiochip_labels) == 0);
}

TEST_CASE("pick_gpiochip returns -1 when no known header chip exists") {
    std::vector<std::string> chips = {"some-other-gpio", "another-chip"};
    REQUIRE(pick_gpiochip(chips, profile_for(PiModel::Unknown).gpiochip_labels) == -1);
    REQUIRE(pick_gpiochip({}, profile_for(PiModel::Pi5).gpiochip_labels) == -1);
}

// ---------------------------------------------------------------
// Rotary encoder events-per-detent
// ---------------------------------------------------------------

TEST_CASE("rotary events-per-detent is 1 on Pi 5 (measured on hardware)") {
    // Measured 2026-07-25 on the Pi 5 bench: 10 detent clicks produced
    // exactly 10 EV_REL events, all value=+1. The kiosk's accumulator
    // threshold must match or the UI advances only every other click.
    REQUIRE(profile_for(PiModel::Pi5).rotary_events_per_detent == 1);
}

TEST_CASE("rotary events-per-detent stays 2 on Pi 4 (fielded behavior preserved)") {
    // The accumulator has shipped with THRESHOLD=2 since the Pi 4B days
    // ("Require accumulating 2 units (one detent click)"). Not yet
    // re-measured on Pi 4 hardware, so keep the long-standing value —
    // changing it blind would double scroll sensitivity on fielded boxes.
    REQUIRE(profile_for(PiModel::Pi4).rotary_events_per_detent == 2);
}

TEST_CASE("rotary events-per-detent defaults conservatively on unknown boards") {
    // Unknown board (incl. dev machines) keeps the historical default so
    // an unrecognized platform can never regress existing behavior.
    REQUIRE(profile_for(PiModel::Unknown).rotary_events_per_detent == 2);
}

// ---------------------------------------------------------------
// Storage mount detection (movie drive present?)
// ---------------------------------------------------------------

// Real /proc/mounts shape from the Pi 5 with the MOVIES drive attached.
static const char* kMountsWithDrive =
    "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
    "devtmpfs /dev devtmpfs rw,relatime 0 0\n"
    "/dev/sda1 /mnt/ssd ext4 rw,relatime 0 0\n"
    "/dev/mmcblk0p1 /boot/firmware vfat rw,relatime 0 0\n";

static const char* kMountsNoDrive =
    "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
    "devtmpfs /dev devtmpfs rw,relatime 0 0\n"
    "/dev/mmcblk0p1 /boot/firmware vfat rw,relatime 0 0\n";

TEST_CASE("is_path_mounted finds the movie drive when attached") {
    REQUIRE(is_path_mounted(kMountsWithDrive, "/mnt/ssd"));
}

TEST_CASE("is_path_mounted reports false when the drive is absent") {
    REQUIRE_FALSE(is_path_mounted(kMountsNoDrive, "/mnt/ssd"));
}

TEST_CASE("is_path_mounted does not match a prefix of another mount point") {
    // "/mnt/ssd" must not be satisfied by "/mnt/ssd2" — field-exact only,
    // or a differently-named drive would masquerade as the movie drive.
    const char* other = "/dev/sdb1 /mnt/ssd2 ext4 rw,relatime 0 0\n";
    REQUIRE_FALSE(is_path_mounted(other, "/mnt/ssd"));
}

TEST_CASE("is_path_mounted handles empty input") {
    REQUIRE_FALSE(is_path_mounted("", "/mnt/ssd"));
}

// ---------------------------------------------------------------
// Movie-playback service pause (Radarr/Prowlarr/Byparr)
// ---------------------------------------------------------------

TEST_CASE("Pi 5 does NOT pause media services during movie playback") {
    // The pause exists purely to reclaim ~320MB (Radarr ~155 + Prowlarr
    // ~165) on the memory-constrained Pi 4B. Measured on Pi 5
    // 2026-07-26: 1122MB of 2006MB still available during 1080p
    // playback WITH the whole Docker stack running — the pause buys
    // nothing and costs a visible bug (20-40s container restart on exit
    // produces a false "tunnel down" toast and a blank library grid).
    REQUIRE_FALSE(profile_for(PiModel::Pi5).pause_services_during_movie);
}

TEST_CASE("Pi 4 keeps pausing media services during movie playback") {
    // Pi 4B genuinely needed it: hardware-decoder DMA buffers plus the
    // Docker stack produced "frozen frame, then 5-second catch-up burst".
    // Fielded boxes must not regress.
    REQUIRE(profile_for(PiModel::Pi4).pause_services_during_movie);
}

TEST_CASE("unknown boards keep the pause (conservative default)") {
    REQUIRE(profile_for(PiModel::Unknown).pause_services_during_movie);
}

// ---------------------------------------------------------------
// Movie-playback torrent handling: trickle (alt limits) vs full pause
// ---------------------------------------------------------------

TEST_CASE("Pi 5 trickles torrents during movie playback (alt speed limits)") {
    // The Pi 5's SSD library + spare CPU can absorb a ~1.5 MB/s trickle
    // while a movie plays, so downloads keep progressing instead of the
    // swarm being stopped for two hours. PlaybackScreen branches on this
    // field: trickle -> set_alt_speed_limits_enabled(true), else the
    // long-shipped pause_all().
    REQUIRE(profile_for(PiModel::Pi5).trickle_torrents_during_video);
}

TEST_CASE("Pi 4 keeps the full torrent pause during movie playback") {
    // USB-flash media: concurrent random read+write tanks throughput to
    // single-digit MB/s — there is no headroom to trickle into. Fielded
    // behavior must not change.
    REQUIRE_FALSE(profile_for(PiModel::Pi4).trickle_torrents_during_video);
}

TEST_CASE("unknown boards get the full pause, not the trickle (conservative)") {
    // Note the flag is movie-scoped by design: GameQuietMode in main.cpp
    // calls pause_all() unconditionally on every board — games need the
    // CPU/RAM back, not just disk quiet — so no profile field softens the
    // game-time pause, and none should be added.
    REQUIRE_FALSE(profile_for(PiModel::Unknown).trickle_torrents_during_video);
}

// ---------------------------------------------------------------
// Game-system support gating (one golden image, two boards)
// ---------------------------------------------------------------

TEST_CASE("normalize_game_system lowercases and strips spaces/quotes") {
    REQUIRE(normalize_game_system("N64") == "n64");
    REQUIRE(normalize_game_system("Dreamcast") == "dreamcast");
    REQUIRE(normalize_game_system("\"PC Engine\"") == "pcengine");
    REQUIRE(normalize_game_system("'Atari 7800'") == "atari7800");
    REQUIRE(normalize_game_system("") == "");
}

TEST_CASE("Pi 4 profile marks N64 and Dreamcast unsupported") {
    PlatformProfile p = profile_for(PiModel::Pi4);
    REQUIRE_FALSE(supports_game_system(p, "N64"));
    REQUIRE_FALSE(supports_game_system(p, "n64"));
    REQUIRE_FALSE(supports_game_system(p, "Dreamcast"));
    REQUIRE_FALSE(supports_game_system(p, "\"Dreamcast\""));
}

TEST_CASE("Pi 4 profile still supports the original seven systems") {
    PlatformProfile p = profile_for(PiModel::Pi4);
    for (const char* sys : {"NES", "SNES", "Genesis", "PS1", "Arcade",
                            "PC Engine", "Atari 7800"}) {
        INFO("system: " << sys);
        REQUIRE(supports_game_system(p, sys));
    }
}

TEST_CASE("unknown and empty system strings are always supported") {
    // The gate must never hide content it doesn't understand — a future
    // system added to playlists before this table learns about it has to
    // keep working on the board that CAN run it.
    PlatformProfile p = profile_for(PiModel::Pi4);
    REQUIRE(supports_game_system(p, "Saturn"));
    REQUIRE(supports_game_system(p, ""));
}

TEST_CASE("Pi 5 and Unknown profiles hide nothing") {
    REQUIRE(profile_for(PiModel::Pi5).unsupported_game_systems.empty());
    REQUIRE(profile_for(PiModel::Unknown).unsupported_game_systems.empty());
    REQUIRE(supports_game_system(profile_for(PiModel::Pi5), "N64"));
    REQUIRE(supports_game_system(profile_for(PiModel::Pi5), "Dreamcast"));
}

// ---------------------------------------------------------------
// service_quiet_mode — memory-gated pause-vs-trickle decision
// ---------------------------------------------------------------
// Regression coverage for the 2026-08-11 playback-stutter diagnosis:
// the Pi 5 profile's static "skip the pause" call was made against a
// July memory measurement that the growing service stack invalidated
// (768 MB in zram swap, 300k major faults in the kiosk during a movie).
// The decision must consult the ACTUAL MemAvailable at session start,
// not just the board model.

TEST_CASE("service_quiet_mode: pause-profile boards full-pause even with plenty of memory") {
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi4), 2'000'000L) ==
            ServiceQuietMode::FullPause);
    REQUIRE(service_quiet_mode(profile_for(PiModel::Unknown), 2'000'000L) ==
            ServiceQuietMode::FullPause);
}

TEST_CASE("service_quiet_mode: trickle board with headroom trickles") {
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5),
                               kServiceQuietMemFloorKiB) ==
            ServiceQuietMode::Trickle);
    // A 4 GB board with the stack resident sits ~2.5 GiB available.
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5), 2'500'000L) ==
            ServiceQuietMode::Trickle);
}

TEST_CASE("service_quiet_mode: trickle board below the memory floor falls back to full pause") {
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5),
                               kServiceQuietMemFloorKiB - 1) ==
            ServiceQuietMode::FullPause);
    // A 2 GB board tops out ~1.3 GiB available even freshly booted — it
    // must ALWAYS land here. Hardware-measured 2026-08-11: at 940 MiB
    // available, Trickle still froze playback ~1/min from resident
    // service ticks alone; only the full pause ran clean.
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5), 1'300'000L) ==
            ServiceQuietMode::FullPause);
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5), 940L * 1024) ==
            ServiceQuietMode::FullPause);
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5), 100'000L) ==
            ServiceQuietMode::FullPause);
}

TEST_CASE("service_quiet_mode: unreadable MemAvailable is conservative (full pause)") {
    REQUIRE(service_quiet_mode(profile_for(PiModel::Pi5), -1L) ==
            ServiceQuietMode::FullPause);
}

// ---------------------------------------------------------------
// parse_mem_available_kib / read_mem_available_kib
// ---------------------------------------------------------------

TEST_CASE("parse_mem_available_kib extracts the MemAvailable line") {
    const std::string meminfo =
        "MemTotal:        2054256 kB\n"
        "MemFree:           54388 kB\n"
        "MemAvailable:     715968 kB\n"
        "Buffers:            5980 kB\n";
    REQUIRE(parse_mem_available_kib(meminfo) == 715968L);
}

TEST_CASE("parse_mem_available_kib: missing or malformed line yields -1") {
    REQUIRE(parse_mem_available_kib("") == -1);
    REQUIRE(parse_mem_available_kib("MemTotal: 2054256 kB\n") == -1);
    REQUIRE(parse_mem_available_kib("MemAvailable: soon kB\n") == -1);
}

TEST_CASE("read_mem_available_kib reads a meminfo-format file; missing file yields -1") {
    auto path = std::filesystem::temp_directory_path() /
                "mdb_test_meminfo.txt";
    {
        std::ofstream f(path);
        f << "MemTotal:  2054256 kB\nMemAvailable:  432100 kB\n";
    }
    REQUIRE(read_mem_available_kib(path.string()) == 432100L);
    std::remove(path.string().c_str());
    REQUIRE(read_mem_available_kib("/nonexistent/meminfo") == -1);
}

// ---------------------------------------------------------------
// Mac host (mdb_headless spec §5.2)
// ---------------------------------------------------------------

TEST_CASE("Mac profile gates nothing off and never quiets services") {
    PlatformProfile p = profile_for(PiModel::Mac);
    REQUIRE(p.model == PiModel::Mac);
    REQUIRE(p.has_analog_audio);
    REQUIRE(p.unsupported_game_systems.empty());
    REQUIRE_FALSE(p.pause_services_during_movie);
    REQUIRE_FALSE(p.trickle_torrents_during_video);
    REQUIRE(p.rotary_events_per_detent == 2);
    REQUIRE(p.gpiochip_labels.empty());
    REQUIRE(supports_game_system(p, "N64"));
    REQUIRE(supports_game_system(p, "Dreamcast"));
}

TEST_CASE("host_model_name is stable wire vocabulary") {
    REQUIRE(std::string(host_model_name(PiModel::Pi4)) == "pi4");
    REQUIRE(std::string(host_model_name(PiModel::Pi5)) == "pi5");
    REQUIRE(std::string(host_model_name(PiModel::Mac)) == "mac");
    REQUIRE(std::string(host_model_name(PiModel::Unknown)) == "unknown");
}

#ifdef __APPLE__
TEST_CASE("detect_platform reports Mac on an Apple host even with a Pi model file") {
    const auto tmp = std::filesystem::temp_directory_path() / "mdb_model_pi5.txt";
    { std::ofstream f(tmp); f << "Raspberry Pi 5 Model B Rev 1.0"; }
    REQUIRE(detect_platform(tmp.string()).model == PiModel::Mac);
    std::filesystem::remove(tmp);
}
#endif
