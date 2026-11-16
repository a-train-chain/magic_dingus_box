// app/tube_build_guard.h - a marker that survives a kiosk killed inside the GPU
// driver's shader compiler (Mesa has hung there; the watchdog then kills the
// kiosk). begin() writes the names of the programs about to be built; end()
// removes the marker. A kiosk that starts and finds the marker knows the last
// build never finished and falls back to the old engine instead of hanging again.
#pragma once

#include <string>
#include <string_view>

namespace app {

class TubeBuildGuard {
public:
    // /dev/shm is memory-backed: the marker survives a kiosk restart, not a reboot.
    static constexpr const char* kDefaultPath = "/dev/shm/mdb-tube-build";

    // Reads the marker once and never throws. Anything at the path - a file, an empty
    // file, an unreadable file, a directory - means tripped: the dangerous case is a
    // kiosk that died mid-write, which leaves exactly an empty or partial file. A path
    // that does not exist (or whose parent is not a directory) is clean. A trip is
    // deliberately sticky: nothing clears a marker this process did not arm, so Tube
    // stays off until the next REBOOT. tripped_program() is the content made safe to
    // log (printable ASCII, 256 characters at most), or "unknown" when there is none.
    explicit TubeBuildGuard(std::string marker_path);

    // Stays true for the life of the object, whatever end() or anyone else does to the file.
    bool tripped() const { return tripped_; }
    const std::string& tripped_program() const { return program_; }

    // A tripped guard never writes: the marker and tripped_program() stay as they were
    // (one WARN). Otherwise writes, flushes and closes the marker before returning. A write error logs a
    // WARN and continues (a build with no marker is the pre-guard behaviour); a file it
    // opened but could not finish writing is removed (and the guard disarmed), so a
    // partial marker never trips the next start. An open that fails (out of file
    // descriptors, a directory at the path) touches nothing and leaves the guard as it
    // was: armed, end() still removes the marker an earlier begin() wrote. This is
    // a file write on the main thread, a rare bounded exception to the no-writes rule:
    // /dev/shm is RAM, so it never touches the SD card.
    void begin(std::string_view programs);

    // Removes the marker only if this instance's begin() wrote it and the path is still
    // a regular file holding exactly the text begin() last wrote successfully (identity
    // by CONTENT: Linux reuses inode numbers at once, so device + inode cannot tell our
    // file from a look-alike). A tripped guard, one that never began, or a path now
    // holding a directory or a file with other content removes nothing. A file with our
    // exact content written by someone else is indistinguishable and removed: it would
    // only claim a build in progress that has ended.
    void end();

private:
    std::string path_;
    std::string program_;
    bool tripped_ = false;
    bool armed_ = false;
    // The text the last successful begin() wrote; end() removes only a file holding it.
    std::string written_;
};

}  // namespace app
