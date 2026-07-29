#include "media_browser/movie_drive.h"

#include <fstream>
#include <sstream>

namespace media_browser {
namespace {

// /proc/mounts escapes characters that would otherwise break field splitting:
// space as \040, tab \011, newline \012, backslash \134.
std::string unescape_mount_field(const std::string& field) {
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size() &&
            field[i + 1] >= '0' && field[i + 1] <= '7' &&
            field[i + 2] >= '0' && field[i + 2] <= '7' &&
            field[i + 3] >= '0' && field[i + 3] <= '7') {
            const int value = (field[i + 1] - '0') * 64 +
                              (field[i + 2] - '0') * 8 +
                              (field[i + 3] - '0');
            out.push_back(static_cast<char>(value));
            i += 3;
            continue;
        }
        out.push_back(field[i]);
    }
    return out;
}

// "/mnt/ssd/" and "/mnt/ssd" name the same drive; callers get this from config
// so both spellings turn up. Root ("/") is left alone.
std::string strip_trailing_slash(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

}  // namespace

bool movie_drive_mounted(const std::string& proc_mounts,
                         const std::string& mountpoint) {
    const std::string wanted = strip_trailing_slash(mountpoint);
    if (wanted.empty()) {
        return false;
    }

    std::istringstream lines(proc_mounts);
    std::string line;
    while (std::getline(lines, line)) {
        // Field 2 is the mountpoint. Comparing against the whole line would
        // match a DEVICE or filesystem type containing the path, and a plain
        // prefix test would accept /mnt/ssd_backup or /mnt/ssd/library as
        // proof that /mnt/ssd itself is mounted — which is exactly backwards,
        // since those can be present while the drive is absent.
        std::istringstream fields(line);
        std::string device;
        std::string where;
        std::string fstype;
        if (!(fields >> device >> where >> fstype)) {
            continue;  // blank or ragged line
        }
        if (strip_trailing_slash(unescape_mount_field(where)) != wanted) {
            continue;
        }
        // /etc/fstab mounts the movie drive with x-systemd.automount, which
        // puts a PERMANENT autofs entry at this mountpoint whether or not a
        // drive is attached:
        //
        //   systemd-1 /mnt/ssd autofs ...   <- always there
        //   /dev/sda1 /mnt/ssd ext4   ...   <- only when connected
        //
        // Accepting the autofs line would report "drive connected" on a box
        // with no drive at all — the exact state this function exists to
        // detect. Only a real filesystem counts.
        if (fstype == "autofs") {
            continue;
        }
        return true;
    }
    return false;
}

bool movie_drive_mounted_now(const std::string& mountpoint) {
    std::ifstream mounts("/proc/mounts");
    if (!mounts) {
        return false;  // fail toward the warning
    }
    std::ostringstream buffer;
    buffer << mounts.rdbuf();
    return movie_drive_mounted(buffer.str(), mountpoint);
}

}  // namespace media_browser
