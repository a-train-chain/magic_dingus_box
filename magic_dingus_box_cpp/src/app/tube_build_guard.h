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
    // kiosk that died mid-write, which leaves exactly an empty or partial file.
    // tripped_program() is then the content, or "unknown" when there is none.
    explicit TubeBuildGuard(std::string marker_path);

    // Stays true for the life of the object, whatever end() or anyone else does to the file.
    bool tripped() const { return tripped_; }
    const std::string& tripped_program() const { return program_; }

    // Writes, flushes and closes the marker before returning. A write error logs a
    // WARN and continues (a build with no marker is the pre-guard behaviour). This is
    // a file write on the main thread, a rare bounded exception to the no-writes rule:
    // /dev/shm is RAM, so it never touches the SD card.
    void begin(std::string_view programs);

    // Removes the marker; a missing file is fine.
    void end();

private:
    std::string path_;
    std::string program_;
    bool tripped_ = false;
};

}  // namespace app
