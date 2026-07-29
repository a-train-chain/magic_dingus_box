// controller_probe -- on-Pi ground-truth check for joydev_index.
//
// For every /dev/input/event* node that looks like a joystick it prints the
// device identity, its ordered EV_KEY / EV_ABS capability lists (with each
// axis's reported range, which is what distinguishes a real hat from an
// 8-bit ABS_X/Y d-pad overload), and the joydev_index-computed RetroArch bind
// token for every button and every axis direction.
//
// Then, for each built-in PhysicalProfile whose evdev codes are ALL present
// on the device, it recomputes every token FROM THE LIVE CAPABILITY LISTS and
// diffs it against the profile's hand-transcribed token. That diff is the
// point of this tool: the unit tests assert those tokens from a transcribed
// capability list, this asserts them from real hardware. Exit status is 0
// only when every applicable profile matches token-for-token.
//
// Read-only: opens devices O_RDONLY|O_NONBLOCK and never reads an event, so
// it is safe to run while the kiosk (or RetroArch) is using the same pads.
//
// Usage:  ./controller_probe [/dev/input/eventN ...]
//   With no arguments it scans all of /dev/input. Needs read access to the
//   event nodes (group `input`, or run under sudo).

#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "retroarch/controller_profile.h"
#include "retroarch/joydev_index.h"
#include "retroarch/logical_controls.h"

namespace {

const char* kind_name(retroarch::PhysicalBinding::Kind k) {
    switch (k) {
        case retroarch::PhysicalBinding::Kind::BUTTON: return "BUTTON";
        case retroarch::PhysicalBinding::Kind::HAT:    return "HAT";
        case retroarch::PhysicalBinding::Kind::AXIS:   return "AXIS";
    }
    return "?";
}

bool has(const std::vector<uint16_t>& v, uint16_t c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

// A profile applies to this device only if every code it binds is actually
// advertised by the device, for the right event type. Anything less and the
// comparison would be meaningless rather than a finding.
bool profile_applies(const retroarch::PhysicalProfile& p,
                     const std::vector<uint16_t>& keys,
                     const std::vector<uint16_t>& abses,
                     std::string* why_not) {
    for (const auto& [control, b] : p.controls) {
        const bool ok = (b.kind == retroarch::PhysicalBinding::Kind::BUTTON)
                            ? has(keys, b.code)
                            : has(abses, b.code);
        if (!ok) {
            if (why_not) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s wants %s code 0x%03x, device does not report it",
                         retroarch::logical_control_key(control), kind_name(b.kind), b.code);
                *why_not = buf;
            }
            return false;
        }
    }
    return true;
}

// Returns the number of MISMATCHED tokens (0 == clean).
int cross_check(const retroarch::PhysicalProfile& p,
                const std::vector<uint16_t>& keys,
                const std::vector<uint16_t>& abses) {
    printf("\n  -- cross-check vs builtin profile \"%s\" (%04x:%04x) --\n",
           p.name.c_str(), p.vid, p.pid);
    std::string why_not;
    if (!profile_applies(p, keys, abses, &why_not)) {
        printf("     NOT APPLICABLE to this device: %s\n", why_not.c_str());
        printf("     (skipped -- this pad is a different shape, not a failure)\n");
        return 0;
    }
    int mismatches = 0, total = 0;
    for (const auto& [control, b] : p.controls) {
        const std::string live =
            retroarch::bind_token(keys, abses, b.kind, b.code, b.direction);
        const bool ok = (live == b.token);
        if (!ok) ++mismatches;
        ++total;
        printf("     %-16s %-6s code=%-5u dir=%+d  profile=%-8s live=%-8s %s\n",
               retroarch::logical_control_key(control), kind_name(b.kind),
               static_cast<unsigned>(b.code), b.direction,
               ("\"" + b.token + "\"").c_str(), ("\"" + live + "\"").c_str(),
               ok ? "MATCH" : "*** MISMATCH ***");
    }
    printf("     RESULT: %d/%d tokens match%s\n", total - mismatches, total,
           mismatches ? "  <<< PROFILE AND LIVE HARDWARE DISAGREE" : "");
    return mismatches;
}

int probe_one(const char* path) {
    const int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;  // not readable / not present -- not a failure
    struct libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        close(fd);
        return 0;
    }
    // Joystick-ish: must have absolute axes. Keyboards/mice/GPIO buttons out.
    if (!libevdev_has_event_type(dev, EV_ABS)) {
        libevdev_free(dev);
        close(fd);
        return 0;
    }

    const char* uniq = libevdev_get_uniq(dev);
    printf("=== %s ===\n", path);
    printf("  name : %s\n", libevdev_get_name(dev));
    printf("  ids  : vid=%04x pid=%04x bus=%04x ver=%04x\n",
           libevdev_get_id_vendor(dev), libevdev_get_id_product(dev),
           libevdev_get_id_bustype(dev), libevdev_get_id_version(dev));
    printf("  uniq : %s\n", uniq ? uniq : "(none)");

    std::vector<uint16_t> keys, abses;
    for (unsigned c = 0x100; c <= 0x2ff; ++c)
        if (libevdev_has_event_code(dev, EV_KEY, c)) keys.push_back(static_cast<uint16_t>(c));
    for (unsigned c = 0; c <= 0x3f; ++c)
        if (libevdev_has_event_code(dev, EV_ABS, c)) abses.push_back(static_cast<uint16_t>(c));

    printf("  EV_KEY codes (%zu):", keys.size());
    for (uint16_t k : keys) printf(" 0x%03x", k);
    printf("\n  EV_ABS codes (%zu):", abses.size());
    for (uint16_t a : abses) printf(" 0x%02x", a);
    printf("\n");

    printf("\n  -- computed tokens --\n");
    for (uint16_t k : keys)
        printf("     key 0x%03x (%3u) -> btn \"%s\"\n", k, k,
               retroarch::bind_token(keys, abses,
                                     retroarch::PhysicalBinding::Kind::BUTTON, k, 0)
                   .c_str());
    for (uint16_t a : abses) {
        const int h = retroarch::hat_number(a);
        const bool is_hat = h >= 0;
        // Range matters: a d-pad reported as an 8-bit ABS_X/ABS_Y (min 0,
        // max <= 255) is NOT a hat, and input_manager.cpp keys off exactly
        // that. Print it so the distinction is visible, not inferred.
        printf("     abs 0x%02x (%2u) %-6s range=[%d..%d] flat=%d fuzz=%d"
               "  -> -:\"%s\"  +:\"%s\"\n",
               a, a, is_hat ? "[hat]" : "[axis]",
               libevdev_get_abs_minimum(dev, a), libevdev_get_abs_maximum(dev, a),
               libevdev_get_abs_flat(dev, a), libevdev_get_abs_fuzz(dev, a),
               retroarch::bind_token(keys, abses,
                                     is_hat ? retroarch::PhysicalBinding::Kind::HAT
                                            : retroarch::PhysicalBinding::Kind::AXIS,
                                     a, -1)
                   .c_str(),
               retroarch::bind_token(keys, abses,
                                     is_hat ? retroarch::PhysicalBinding::Kind::HAT
                                            : retroarch::PhysicalBinding::Kind::AXIS,
                                     a, +1)
                   .c_str());
    }

    int mismatches = 0;
    mismatches += cross_check(retroarch::builtin_n64_adapter_profile(), keys, abses);
    mismatches += cross_check(retroarch::builtin_dragonrise_profile(), keys, abses);
    printf("\n");

    libevdev_free(dev);
    close(fd);
    return mismatches;
}

}  // namespace

int main(int argc, char** argv) {
    int mismatches = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) mismatches += probe_one(argv[i]);
    } else {
        DIR* dir = opendir("/dev/input");
        if (!dir) {
            fprintf(stderr, "controller_probe: cannot open /dev/input\n");
            return 2;
        }
        std::vector<std::string> nodes;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) != 0) continue;
            nodes.push_back(std::string("/dev/input/") + entry->d_name);
        }
        closedir(dir);
        // readdir order is arbitrary; sort numerically so runs are diffable.
        std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b) {
            return atoi(a.c_str() + sizeof("/dev/input/event") - 1) <
                   atoi(b.c_str() + sizeof("/dev/input/event") - 1);
        });
        for (const std::string& n : nodes) mismatches += probe_one(n.c_str());
    }
    if (mismatches) {
        printf("controller_probe: %d TOKEN MISMATCH(ES) -- joydev_index and the "
               "builtin profiles disagree on real hardware\n", mismatches);
        return 1;
    }
    printf("controller_probe: all applicable builtin-profile tokens reproduced "
           "from live hardware\n");
    return 0;
}
