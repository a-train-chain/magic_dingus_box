"""
Tests for which block devices the Prepare Drive flow is allowed to erase.

This is the only code in the product that runs mkfs. Getting it wrong does not
produce a bug report — it produces a customer whose Magic Dingus Box no longer
boots, or whose movie library is gone. So the eligibility rule is pure,
separately testable logic rather than something inlined next to the subprocess
call, and the tests below are deliberately adversarial.

The rule the device must satisfy to be offered:

  * it is a whole disk, not a partition
  * it is not the disk the running system booted from
  * nothing on it is mounted at a system path

Everything else — sizes, labels, whether it is USB — is presentation. The
safety decision is only ever "is this the boot disk or serving the OS".

Shape of the input is `lsblk -J -o NAME,TYPE,SIZE,RM,MOUNTPOINT,LABEL,FSTYPE`,
which is what the endpoint actually shells out to.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from storage_prepare import (  # noqa: E402
    PROTECTED_MOUNTPOINTS,
    eligible_devices,
    protected_disk_names,
)


def disk(name, *, size="239G", removable=True, children=None, **kw):
    d = {
        "name": name,
        "type": "disk",
        "size": size,
        "rm": removable,
        "mountpoint": None,
        "label": None,
        "fstype": None,
    }
    d.update(kw)
    if children is not None:
        d["children"] = children
    return d


def part(name, *, mountpoint=None, label=None, fstype="ext4", size="239G"):
    return {
        "name": name,
        "type": "part",
        "size": size,
        "rm": False,
        "mountpoint": mountpoint,
        "label": label,
        "fstype": fstype,
    }


# The real board: SD card carrying / and /boot/firmware, plus a USB movie disk.
PI_LSBLK = {
    "blockdevices": [
        disk("sda", children=[part("sda1", mountpoint="/mnt/ssd", label="MOVIES")]),
        disk(
            "mmcblk0",
            size="59.5G",
            removable=False,
            children=[
                part("mmcblk0p1", mountpoint="/boot/firmware", label="bootfs",
                     fstype="vfat", size="512M"),
                part("mmcblk0p2", mountpoint="/", label="rootfs", size="59G"),
            ],
        ),
        disk("zram0", size="1.9G", removable=False, fstype="swap"),
    ]
}


def test_the_boot_disk_is_never_offered():
    # The single most important assertion in this file. mmcblk0 carries both /
    # and /boot/firmware; offering it would brick the unit.
    names = [d["name"] for d in eligible_devices(PI_LSBLK, protected={"mmcblk0"})]
    assert "mmcblk0" not in names


def test_a_usb_disk_is_offered():
    names = [d["name"] for d in eligible_devices(PI_LSBLK, protected={"mmcblk0"})]
    assert "sda" in names


def test_partitions_are_never_offered_only_whole_disks():
    # Formatting mmcblk0p2 directly would be just as fatal as the whole disk.
    names = [d["name"] for d in eligible_devices(PI_LSBLK, protected={"mmcblk0"})]
    assert not any(n.startswith("mmcblk0p") for n in names)
    assert "sda1" not in names


def test_zram_and_loop_devices_are_never_offered():
    tricky = {
        "blockdevices": [
            disk("zram0", size="1.9G", removable=False, fstype="swap"),
            disk("loop0", size="100M", removable=False),
            disk("sdb"),
        ]
    }
    names = [d["name"] for d in eligible_devices(tricky, protected=set())]
    assert names == ["sdb"]


def test_a_disk_serving_a_system_path_is_refused_even_if_not_named_protected():
    # Belt and braces: if the caller's protected set is somehow wrong, a disk
    # with a child mounted at / or /boot/firmware must still be refused.
    rogue = {
        "blockdevices": [
            disk("sdz", children=[part("sdz1", mountpoint="/")]),
            disk("sdy", children=[part("sdy1", mountpoint="/boot/firmware")]),
            disk("sdx", children=[part("sdx1", mountpoint="/mnt/ssd")]),
        ]
    }
    names = [d["name"] for d in eligible_devices(rogue, protected=set())]
    assert names == ["sdx"]


def test_every_protected_mountpoint_is_actually_enforced():
    # Guards the list itself: adding a path to PROTECTED_MOUNTPOINTS without
    # the filter honouring it would be a silent hole.
    for mp in PROTECTED_MOUNTPOINTS:
        data = {"blockdevices": [disk("sdq", children=[part("sdq1", mountpoint=mp)])]}
        assert eligible_devices(data, protected=set()) == [], f"{mp} not enforced"


def test_an_empty_unformatted_disk_is_offered():
    # The actual customer case: a brand-new drive with no partition table.
    fresh = {"blockdevices": [disk("sdb", size="64G")]}
    names = [d["name"] for d in eligible_devices(fresh, protected=set())]
    assert names == ["sdb"]


def test_the_current_movies_drive_is_flagged_so_the_ui_can_warn():
    devs = eligible_devices(PI_LSBLK, protected={"mmcblk0"})
    sda = next(d for d in devs if d["name"] == "sda")
    assert sda["is_current_movies_drive"] is True


def test_a_fresh_disk_is_not_flagged_as_the_movies_drive():
    fresh = {"blockdevices": [disk("sdb", size="64G")]}
    dev = eligible_devices(fresh, protected=set())[0]
    assert dev["is_current_movies_drive"] is False


def test_devices_report_a_stable_path_for_the_caller_to_format():
    dev = eligible_devices({"blockdevices": [disk("sdb")]}, protected=set())[0]
    assert dev["path"] == "/dev/sdb"


def test_malformed_lsblk_output_yields_nothing_rather_than_guessing():
    # A parse failure must never be interpreted as "everything is eligible".
    for junk in ({}, {"blockdevices": None}, {"blockdevices": [{}]}, None):
        assert eligible_devices(junk, protected=set()) == []


def test_protected_disk_names_maps_a_partition_back_to_its_disk():
    # findmnt reports /dev/mmcblk0p2 for /; the thing we must protect is the
    # whole disk mmcblk0, not just that partition.
    assert protected_disk_names(["/dev/mmcblk0p2", "/dev/mmcblk0p1"]) == {"mmcblk0"}
    assert protected_disk_names(["/dev/sda1"]) == {"sda"}
    assert protected_disk_names(["/dev/nvme0n1p2"]) == {"nvme0n1"}


def test_protected_disk_names_survives_junk_from_findmnt():
    # findmnt can return nothing, or something that is not a device path, on a
    # box in a strange state. That must not silently produce an empty protected
    # set that makes the boot disk eligible — the caller treats an empty result
    # as fatal, and these must not raise.
    assert protected_disk_names([]) == set()
    assert protected_disk_names(["", None, "tmpfs", "/dev/"]) == set()


# --- gating -------------------------------------------------------------
# The Prepare Drive flow must be invisible until the kiosk's secret sequence
# has been entered, exactly like the rest of the Media Browser. A destructive
# endpoint reachable on a locked box would be the worst kind of hole: nothing
# in the UI would hint it exists.

def test_prepare_endpoints_are_refused_while_the_media_browser_is_locked(client):
    # A stock box has never had the unlock sequence entered.
    listing = client.get("/admin/media-browser/storage/devices")
    assert listing.status_code == 403
    assert listing.get_json()["error"]["code"] == "media_browser_locked"

    fmt = client.post("/admin/media-browser/storage/prepare",
                      json={"device": "sda", "confirm": "sda"})
    # 403 (locked) or 400 (CSRF rejected first) — never 200, and never a format.
    assert fmt.status_code in (400, 403)
    assert fmt.status_code != 200


def test_the_destructive_endpoint_is_a_post_not_a_get(client):
    # A GET that formats a drive could be triggered by a prefetch or a stray
    # link. Assert the method surface explicitly.
    assert client.get("/admin/media-browser/storage/prepare").status_code in (403, 405)
