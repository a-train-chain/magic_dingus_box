"""Upload writes that leave the SD card to the kiosk's video reads.

The Content Manager and the kiosk share one SD card, and the kiosk reads
the video it is playing from it a few hundred KB at a time. A read that
waits seconds is a frozen picture; one over 3 s makes the Channel Guide
reload the channel, and a reload that blocks long enough has the systemd
watchdog kill the kiosk. A 2.57 GB upload did all three on a Pi 5
(magicpi5, 2026-11-15; docs/superpowers/validation/
2026-11-15-upload-sd-contention.md):

  * werkzeug spooled the request body to the card through the page cache:
    100+ MB of dirty pages, written back by the kernel's flusher as fast
    as the card takes them;
  * the route then COPIED the whole spool to a second file before ffmpeg
    started: 2.57 GB read back from the card and written again;
  * nothing ranks the kiosk's reads above that. The kiosk unit's
    IOWeight=1000 does nothing on the Pi kernel (no io.weight anywhere: no
    io.cost, no BFQ group scheduling), an I/O class on the uploader cannot
    reach the flusher's writes, and neither the realtime class for the
    reader nor the BFQ scheduler fixed it (measured). What the reads wait
    behind is how much write the card already has queued.

So the uploader keeps that small itself:

  SpoolFile           the request body spools to a NAMED file in
                      upload_temp, written back every
                      MAGIC_UPLOAD_FLUSH_MIB (default 1) with
                      sync_file_range and then dropped from the page cache:
                      the card never has more than one slice of this upload
                      queued, and the kiosk's own cached pages stay put.
  save_upload()       a route that keeps the upload hard-links the spool
                      to its own name instead of copying it (same
                      filesystem: upload_temp and media are both under
                      data/). Anything else (an in-memory upload, a link
                      the filesystem refuses) is copied as before.
  idle_io / background_io
                      the upload thread's I/O (the slices it writes back
                      itself) runs in the IDLE class, which mq-deadline
                      dispatches after waiting normal-class requests.
                      Restored when the route returns: gunicorn reuses its
                      threads.

The storage precondition is unchanged: the spool is still a full copy of
the body for the length of the request, and the transcode still needs
room for its .part beside the original.
"""
from __future__ import annotations

import contextlib
import ctypes
import ctypes.util
import functools
import io
import os
import platform
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Optional

from flask import Request, current_app

MiB = 1024 * 1024

# Spool file names, so the startup sweep and a person reading upload_temp
# can tell them apart from the transcode inputs linked off them.
SPOOL_PREFIX = "spool_"

# Requests at or below this size keep werkzeug's in-memory spool: they are
# forms and small files, never the problem, and a file per request would
# only add writes.
SMALL_REQUEST_BYTES = 1 * MiB

# One slice of the spool written back at a time: the most this upload ever
# has queued at the card, so the longest a kiosk read waits behind it.
# Measured on magicpi5 (Pi 5, SanDisk 64 GB SD, 1 GiB written, idle class;
# worst wait of a kiosk-like reader): unpaced 4.2 s, 8 MiB 1.3 s, 2 MiB
# 1.3 s, 1 MiB 0.35 s, 512 KiB 0.66 s. 1 MiB slices still write 10.7 MiB/s,
# and a real Wi-Fi upload spooled at 10.4 MiB/s with them.
DEFAULT_FLUSH_MIB = 1


def flush_every_bytes() -> int:
    """MAGIC_UPLOAD_FLUSH_MIB: MiB per write-back slice; 0 turns pacing off
    (the spool is then written like werkzeug's own temp file)."""
    raw = os.environ.get("MAGIC_UPLOAD_FLUSH_MIB")
    if raw is None:
        return DEFAULT_FLUSH_MIB * MiB
    try:
        mib = int(raw)
    except ValueError:
        return DEFAULT_FLUSH_MIB * MiB
    if mib < 0:
        return DEFAULT_FLUSH_MIB * MiB
    return mib * MiB


# ------------------------------------------------------------ OS primitives

_libc = None
if sys.platform.startswith("linux"):
    try:
        _libc = ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6",
                            use_errno=True)
        _libc.sync_file_range.argtypes = [ctypes.c_int, ctypes.c_int64,
                                          ctypes.c_int64, ctypes.c_uint]
        _libc.sync_file_range.restype = ctypes.c_int
    except (OSError, AttributeError):
        _libc = None

_SFR_WAIT_BEFORE, _SFR_WRITE, _SFR_WAIT_AFTER = 1, 2, 4


def _writeback(fd: int, offset: int, length: int) -> None:
    """Write [offset, offset+length) to the device and wait for it.

    sync_file_range, not fdatasync: it starts and waits for exactly this
    range's data and nothing else — no journal commit, no device cache
    flush — so it paces the writer without adding writes of its own.
    """
    if _libc is not None:
        if _libc.sync_file_range(fd, offset, length, _SFR_WAIT_BEFORE
                                 | _SFR_WRITE | _SFR_WAIT_AFTER) == 0:
            return
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    os.fsync(fd)  # not Linux (a dev machine): correct, just heavier


def _drop_cache(fd: int, offset: int, length: int) -> None:
    """Drop a written-back range from the page cache. Nothing reads the
    spool again before the request ends except a copy fallback or ffmpeg,
    and either streams it from the card at its own pace."""
    if hasattr(os, "posix_fadvise"):
        os.posix_fadvise(fd, offset, length, os.POSIX_FADV_DONTNEED)


# ioprio_set/ioprio_get(IOPRIO_WHO_PROCESS, 0, ...) act on the calling
# THREAD. Python has no wrapper; the syscall numbers are per architecture.
_IOPRIO_SYSCALLS = {
    "aarch64": (30, 31),    # asm-generic: arm64 (every shipped box)
    "arm64": (30, 31),
    "x86_64": (251, 252),
    "armv7l": (314, 315),   # 32-bit ARM EABI userland
    "armv6l": (314, 315),
}
_IOPRIO_WHO_PROCESS = 1
_IOPRIO_CLASS_SHIFT = 13
IOPRIO_IDLE = 3 << _IOPRIO_CLASS_SHIFT


def _ioprio_numbers():
    if _libc is None:
        return None
    return _IOPRIO_SYSCALLS.get(platform.machine())


def _ioprio_get() -> Optional[int]:
    """The calling thread's I/O priority, or None where it can't be read."""
    nums = _ioprio_numbers()
    if nums is None:
        return None
    value = _libc.syscall(nums[1], _IOPRIO_WHO_PROCESS, 0)
    return None if value < 0 else value


def _ioprio_set(value: int) -> None:
    nums = _ioprio_numbers()
    if nums is None:
        return
    if _libc.syscall(nums[0], _IOPRIO_WHO_PROCESS, 0, value) != 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))


@contextlib.contextmanager
def idle_io():
    """Run the block with this thread's I/O in the idle class.

    Best effort: where I/O priorities can't be read or set, the block runs
    unchanged. Always restores the previous priority — the thread goes back
    to gunicorn's pool and serves the next request.
    """
    previous = _ioprio_get()
    if previous is None:
        yield
        return
    try:
        _ioprio_set(IOPRIO_IDLE)
    except OSError as e:
        print(f"[upload_io] could not lower upload I/O priority: {e}",
              file=sys.stderr)
        yield
        return
    try:
        yield
    finally:
        try:
            _ioprio_set(previous)
        except OSError as e:
            print(f"[upload_io] could not restore I/O priority: {e}",
                  file=sys.stderr)


def background_io(view):
    """Route decorator: the whole request (spool, link or copy) in idle_io."""
    @functools.wraps(view)
    def wrapper(*args, **kwargs):
        with idle_io():
            return view(*args, **kwargs)
    return wrapper


# --------------------------------------------------------------- the spool

class WriteBackPacer:
    """Keeps one growing file's write-back to a slice at a time.

    Call wrote(f) after each write to `f`. Every `flush_every` bytes it
    pushes the newest slice to the card and waits for it (_writeback), then
    drops it from the page cache, so the card never has more than one slice
    of this file queued ahead of the kiosk's reads and the kiosk's own
    cached pages stay put. Used for upload spools (SpoolFile) and for
    anything else that writes a big file to the SD card in a request — a
    package import's extraction.

    Holds no reference to the file, so a file that owns its pacer is still
    freed (and a spool removed) by reference counting.
    """

    def __init__(self, flush_every: Optional[int] = None):
        self._every = flush_every_bytes() if flush_every is None else flush_every
        self._flushed = 0
        self._failed = False

    def wrote(self, f) -> None:
        if self._every <= 0 or self._failed:
            return
        end = f.tell()
        if end - self._flushed < self._every:
            return
        # Python's own buffer first. Outside the try: a write that fails
        # here (a full card) is the caller's error, never swallowed.
        f.flush()
        try:
            _writeback(f.fileno(), self._flushed, end - self._flushed)
            _drop_cache(f.fileno(), self._flushed, end - self._flushed)
        except OSError as e:
            # Pacing is a courtesy to the kiosk, never a reason to fail the
            # write: fall back to the kernel's own write-back.
            self._failed = True
            print(f"[upload_io] write pacing off for this file: {e}",
                  file=sys.stderr)
        self._flushed = end


class SpoolFile(io.FileIO):
    """A request body's file part, spooled to a named file and paced.

    Writes go straight to the file (unbuffered: werkzeug already writes in
    64 KiB pieces) and are paced by a WriteBackPacer. The file is removed
    when it is closed — at the end of the request — unless a route has
    linked it to a name of its own first.
    """

    def __init__(self, directory, flush_every: Optional[int] = None):
        self.path = Path(directory) / f"{SPOOL_PREFIX}{uuid.uuid4().hex}"
        super().__init__(str(self.path), "x+")
        self._pacer = WriteBackPacer(flush_every)

    def write(self, b) -> int:
        view = memoryview(b).cast("B")
        done = 0
        while done < len(view):
            done += super().write(view[done:])
        self._pacer.wrote(self)
        return done

    def link_to(self, dest) -> bool:
        """Give the spooled bytes a second name; False if the filesystem
        refuses (another filesystem, or no hard links)."""
        try:
            os.link(self.path, dest)
            return True
        except OSError:
            return False

    def close(self) -> None:
        if self.closed:
            return
        try:
            super().close()
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(self.path)


def save_upload(file_storage, dest) -> None:
    """Put an upload's bytes at `dest`, which must not exist yet.

    A spooled upload is hard-linked: no second copy is written or read. The
    spool's own name goes with the request. Anything else is copied
    (FileStorage.save), exactly as before.
    """
    stream = getattr(file_storage, "stream", None)
    if isinstance(stream, SpoolFile) and stream.link_to(dest):
        return
    file_storage.save(str(dest))


class PacedUploadRequest(Request):
    """Flask request class: large file parts spool to a SpoolFile in
    UPLOAD_SPOOL_DIR (upload_temp) instead of werkzeug's anonymous temp
    file."""

    def _get_file_stream(self, total_content_length, content_type,
                         filename=None, content_length=None):
        if (total_content_length is not None
                and total_content_length <= SMALL_REQUEST_BYTES):
            return super()._get_file_stream(
                total_content_length, content_type, filename, content_length)
        spool_dir = (current_app.config.get("UPLOAD_SPOOL_DIR")
                     or tempfile.gettempdir())
        return SpoolFile(spool_dir)


__all__ = [
    "DEFAULT_FLUSH_MIB", "IOPRIO_IDLE", "PacedUploadRequest", "SPOOL_PREFIX",
    "SpoolFile", "WriteBackPacer", "background_io", "flush_every_bytes",
    "idle_io", "save_upload",
]
