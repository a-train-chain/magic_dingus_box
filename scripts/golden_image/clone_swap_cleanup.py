#!/usr/bin/env python3
"""Clear allocated, inactive swap before cloning (free-space fills miss it).

Only known files on the root filesystem are eligible. Disk swap must stay
inactive throughout clone preparation/capture; this tool never calls swapoff.
The source retains a valid swap file of the same size and UUID. Contents of
inactive swap are disposable, so no secret-bearing backup is made.
"""
import argparse
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import uuid


def check_active(swaps):
    lines = swaps.read_text().splitlines()
    if not lines or lines[0].split() != ['Filename', 'Type', 'Size', 'Used', 'Priority']:
        raise RuntimeError('cannot validate active swap inventory')
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != 5:
            raise RuntimeError('cannot validate active swap inventory')
        if not re.fullmatch(r'/dev/zram[0-9]+', fields[0]) or fields[1] != 'partition':
            raise RuntimeError('active disk swap: disable it safely before cloning; no swap was modified')


def candidates(root):
    paths = {'/var/swap', '/swapfile', '/swap.img'}
    for line in (root / 'etc/fstab').read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith('#') or len(fields) < 3 or fields[2] != 'swap':
            continue
        source = re.sub(r'\\([0-7]{3})', lambda match: chr(int(match[1], 8)), fields[0])
        if re.fullmatch(r'/dev/zram[0-9]+', source):
            continue
        if not source.startswith('/') or source.startswith('/dev/') or '..' in PurePosixPath(source).parts:
            raise RuntimeError('unsupported fstab swap source: inspect disk swap before cloning')
        paths.add(source)
    return [root / path.lstrip('/') for path in sorted(paths)]


def inspect_file(root, path):
    """Reject aliases and foreign mounts before opening anything for writing."""
    root_device = root.stat().st_dev
    current = root
    for component in path.relative_to(root).parts:
        current = current / component
        try:
            info = os.lstat(current)
        except FileNotFoundError:
            return None
        if stat.S_ISLNK(info.st_mode):
            raise RuntimeError('swap candidate contains a symlink; refusing cleanup')
        if info.st_dev != root_device:
            raise RuntimeError('swap candidate is outside the root filesystem; refusing cleanup')
    if not stat.S_ISREG(info.st_mode):
        raise RuntimeError('swap candidate is not a regular file')
    if info.st_nlink != 1:
        raise RuntimeError('swap candidate has a hardlink; refusing cleanup')
    with path.open('rb') as stream:
        header = stream.read(65536)
    if not any(header[size - 10:size] == b'SWAPSPACE2' for size in (4096, 8192, 16384, 65536)):
        raise RuntimeError('swap candidate lacks a recognized swap signature; refusing cleanup')
    return info, str(uuid.UUID(bytes=header[1036:1052]))


def clean_swap(root=Path('/'), swaps=Path('/proc/swaps'), *, check_only=False):
    check_active(swaps)
    # Validate the entire set first: one unsafe candidate must prevent writes
    # to the others as well. No file contents or fstab values enter diagnostics.
    pending = []
    for path in candidates(root):
        result = inspect_file(root, path)
        if result:
            pending.append((path, *result))
    mkswap = shutil.which('mkswap')
    if pending and not mkswap:
        raise RuntimeError('mkswap is required to preserve usable inactive swap')
    if check_only:
        return
    zeros = bytes(1024 * 1024)
    for path, original, swap_uuid in pending:
        check_active(swaps)
        inspected = inspect_file(root, path)
        if not inspected or (inspected[0].st_dev, inspected[0].st_ino) != (original.st_dev, original.st_ino):
            raise RuntimeError('swap candidate changed during preparation')
        fd = os.open(path, os.O_WRONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'wb') as stream:
            current = os.fstat(stream.fileno())
            if (current.st_dev, current.st_ino, current.st_size, current.st_nlink) != (
                    original.st_dev, original.st_ino, original.st_size, 1):
                raise RuntimeError('swap candidate changed during preparation')
            remaining = current.st_size
            while remaining:
                count = min(remaining, len(zeros))
                stream.write(zeros[:count])
                remaining -= count
            stream.flush()
            os.fsync(stream.fileno())
        # util-linux is already required by clone preparation (findmnt/lsblk).
        # Silence formatter diagnostics: paths/labels must never leak values.
        subprocess.run([mkswap, '--uuid', swap_uuid, str(path)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if pending:
        os.sync()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='read-only preflight')
    args = parser.parse_args()
    try:
        clean_swap(check_only=args.check)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError):
        # Deliberately do not print exception text (may include file contents).
        print('ERROR: swap cleanup refused or failed; inspect active disk swap, fstab, and inactive swap paths before cloning.', file=sys.stderr)
        return 1
    print('Swap preflight clean.' if args.check else 'Inactive root swap sanitized; active zram retained.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
