// Is the movie drive actually mounted, or is STORAGE_ROOT a bare directory on
// the SD card?
//
// This distinction is invisible from everywhere else in the product, and it is
// the difference between "downloads go to a 239 GB drive" and "downloads fill
// the 17 GB partition the OS is running from". The kiosk's Storage-path row
// cannot tell them apart today: it reports Radarr's root folder and its free
// space, and when nothing is mounted at /mnt/ssd that free space is the SD
// card's — with nothing to say so.
//
// Deliberately parses /proc/mounts as text rather than calling statfs, so the
// interesting cases (a prefix collision, a stale entry, an unreadable file)
// can be exercised without needing a real block device.

#include <catch2/catch_test_macros.hpp>

#include "media_browser/movie_drive.h"

using media_browser::movie_drive_mounted;

namespace {

// Trimmed from the real box.
const char* kRealMounts =
    "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
    "devtmpfs /dev devtmpfs rw,relatime 0 0\n"
    "/dev/mmcblk0p1 /boot/firmware vfat rw,relatime 0 0\n"
    "/dev/sda1 /mnt/ssd ext4 rw,relatime 0 0\n";

const char* kNoDrive =
    "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
    "devtmpfs /dev devtmpfs rw,relatime 0 0\n"
    "/dev/mmcblk0p1 /boot/firmware vfat rw,relatime 0 0\n";

}  // namespace

TEST_CASE("a mounted movie drive is detected", "[media_browser][storage]") {
    REQUIRE(movie_drive_mounted(kRealMounts, "/mnt/ssd"));
}

TEST_CASE("no drive means not mounted", "[media_browser][storage]") {
    REQUIRE_FALSE(movie_drive_mounted(kNoDrive, "/mnt/ssd"));
}

TEST_CASE("a longer path starting with the mountpoint does not count",
          "[media_browser][storage]") {
    // The bug a naive substring search would produce: /mnt/ssd_backup or
    // /mnt/ssd2 mounted while /mnt/ssd itself is not, reported as fine.
    const char* decoys =
        "/dev/sdb1 /mnt/ssd_backup ext4 rw 0 0\n"
        "/dev/sdc1 /mnt/ssd2 ext4 rw 0 0\n"
        "/dev/sdd1 /mnt/ssd/library ext4 rw 0 0\n";
    REQUIRE_FALSE(movie_drive_mounted(decoys, "/mnt/ssd"));
}

TEST_CASE("the mountpoint must be the second field, not anywhere on the line",
          "[media_browser][storage]") {
    // A device or filesystem type that happens to contain the path must not
    // register as a mount of it.
    const char* weird = "/mnt/ssd /some/other/place ext4 rw 0 0\n";
    REQUIRE_FALSE(movie_drive_mounted(weird, "/mnt/ssd"));
}

TEST_CASE("octal-escaped mountpoints are handled", "[media_browser][storage]") {
    // /proc/mounts escapes spaces as \040. A path containing one would
    // otherwise split into the wrong fields.
    const char* escaped = "/dev/sda1 /mnt/my\\040drive ext4 rw 0 0\n";
    REQUIRE(movie_drive_mounted(escaped, "/mnt/my drive"));
    REQUIRE_FALSE(movie_drive_mounted(escaped, "/mnt/my"));
}

TEST_CASE("unreadable or empty mounts reports NOT mounted",
          "[media_browser][storage]") {
    // Fail toward the warning. A false alarm makes someone check a cable; the
    // opposite silently hides downloads filling the OS partition, which is the
    // failure this whole check exists to surface.
    REQUIRE_FALSE(movie_drive_mounted("", "/mnt/ssd"));
    REQUIRE_FALSE(movie_drive_mounted("garbage with no newlines or fields",
                                      "/mnt/ssd"));
}

TEST_CASE("an empty mountpoint argument never reports mounted",
          "[media_browser][storage]") {
    REQUIRE_FALSE(movie_drive_mounted(kRealMounts, ""));
}

TEST_CASE("a trailing slash on the mountpoint still matches",
          "[media_browser][storage]") {
    // Callers pass this from config; /mnt/ssd and /mnt/ssd/ mean the same
    // drive and must not give different answers.
    REQUIRE(movie_drive_mounted(kRealMounts, "/mnt/ssd/"));
}

TEST_CASE("the systemd automount placeholder is not a mounted drive",
          "[media_browser][storage]") {
    // THE case this check exists for, and the one that nearly shipped broken.
    // /etc/fstab uses x-systemd.automount, so /proc/mounts on the real box
    // carries TWO entries for /mnt/ssd:
    //
    //   systemd-1 /mnt/ssd autofs ...   <- always present, drive or not
    //   /dev/sda1 /mnt/ssd ext4   ...   <- only when a drive is connected
    //
    // Matching on the mountpoint alone reports "connected" on a box with no
    // drive whatsoever — precisely the state we are trying to surface.
    const char* automount_only =
        "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
        "systemd-1 /mnt/ssd autofs rw,relatime,fd=58,pgrp=1,timeout=0 0 0\n";
    REQUIRE_FALSE(movie_drive_mounted(automount_only, "/mnt/ssd"));

    // Both lines present (drive actually connected) must report true.
    const char* automount_and_drive =
        "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
        "systemd-1 /mnt/ssd autofs rw,relatime,fd=58,pgrp=1,timeout=0 0 0\n"
        "/dev/sda1 /mnt/ssd ext4 rw,relatime 0 0\n";
    REQUIRE(movie_drive_mounted(automount_and_drive, "/mnt/ssd"));
}

TEST_CASE("lines without enough fields are skipped rather than misread",
          "[media_browser][storage]") {
    const char* ragged =
        "\n"
        "onlyonefield\n"
        "two fields\n"
        "/dev/sda1 /mnt/ssd ext4 rw 0 0\n";
    REQUIRE(movie_drive_mounted(ragged, "/mnt/ssd"));
}
