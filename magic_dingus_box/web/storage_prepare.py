"""Which block devices the Prepare Drive flow may erase.

This is the only code in the product that leads to mkfs. A mistake here does
not produce a bug report — it produces a customer whose box no longer boots, or
whose movie library is gone. So the eligibility decision lives here as pure
logic over `lsblk -J` output, tested adversarially in
tests/test_storage_prepare.py, rather than inline next to the subprocess call.

Two independent guards, deliberately redundant:

  1. The caller passes the set of disks backing the running system, derived
     from findmnt via protected_disk_names().
  2. Regardless of what the caller passed, any disk with a partition mounted
     at a system path is refused here.

Guard 2 exists because guard 1 depends on findmnt succeeding. If findmnt ever
returns something unexpected the protected set could come back empty, and
without the second check the boot disk would become eligible.
"""
from __future__ import annotations

import re

# Mounts that mean "this disk is serving the running system". A disk carrying
# any of these is never offered, whatever else is true about it.
PROTECTED_MOUNTPOINTS = frozenset({"/", "/boot", "/boot/firmware"})

# Virtual block devices that are never real removable storage.
_VIRTUAL_PREFIXES = ("loop", "zram", "ram", "dm-", "md")

# Where the movie drive mounts, so the UI can warn that this is the drive
# currently holding the library.
_MOVIES_MOUNTPOINT = "/mnt/ssd"
_MOVIES_LABEL = "MOVIES"

# /dev/mmcblk0p2 -> mmcblk0, /dev/nvme0n1p2 -> nvme0n1, /dev/sda1 -> sda.
# The p-suffixed forms matter: naively stripping trailing digits turns
# mmcblk0p2 into mmcblk0p and protects nothing.
_PARTITION_SUFFIX = re.compile(r"^(.*?)(?:p\d+|\d+)$")


def protected_disk_names(sources):
    """Map device paths (as findmnt reports them) to the whole disks behind them.

    findmnt gives us the PARTITION serving a mount (/dev/mmcblk0p2 for /), but
    the thing that must never be formatted is the disk it sits on.

    Returns a set of kernel names. Junk entries are skipped rather than raised
    on — but note an empty result is not a licence to proceed: the caller is
    expected to treat "no protected disks found" as a failure, since it means
    we could not establish what the system is running from.
    """
    disks = set()
    for source in sources or []:
        if not source or not isinstance(source, str):
            continue
        if not source.startswith("/dev/"):
            continue
        name = source[len("/dev/"):].strip()
        if not name:
            continue
        match = _PARTITION_SUFFIX.match(name)
        disks.add(match.group(1) if match else name)
    return disks


def _is_virtual(name):
    return any(name.startswith(prefix) for prefix in _VIRTUAL_PREFIXES)


def _serves_the_system(device):
    """True if this disk or any of its partitions is mounted at a system path."""
    candidates = [device] + list(device.get("children") or [])
    for node in candidates:
        if not isinstance(node, dict):
            continue
        if node.get("mountpoint") in PROTECTED_MOUNTPOINTS:
            return True
    return False


def _is_current_movies_drive(device):
    for node in [device] + list(device.get("children") or []):
        if not isinstance(node, dict):
            continue
        if node.get("mountpoint") == _MOVIES_MOUNTPOINT:
            return True
        if node.get("label") == _MOVIES_LABEL:
            return True
    return False


def eligible_devices(lsblk_json, protected):
    """Whole disks that may safely be offered for formatting.

    A malformed or missing input yields an empty list. That direction matters:
    failing to parse must never be read as "everything is eligible".
    """
    if not isinstance(lsblk_json, dict):
        return []
    blockdevices = lsblk_json.get("blockdevices")
    if not isinstance(blockdevices, list):
        return []

    protected = protected or set()
    out = []
    for device in blockdevices:
        if not isinstance(device, dict):
            continue
        name = device.get("name")
        if not name or not isinstance(name, str):
            continue
        # Whole disks only — never a partition, which would be just as fatal
        # to format as the disk it lives on.
        if device.get("type") != "disk":
            continue
        if _is_virtual(name):
            continue
        if name in protected:
            continue
        if _serves_the_system(device):
            continue

        out.append({
            "name": name,
            "path": "/dev/" + name,
            "size": device.get("size"),
            "removable": bool(device.get("rm")),
            "label": device.get("label"),
            "fstype": device.get("fstype"),
            "is_current_movies_drive": _is_current_movies_drive(device),
        })
    return out
