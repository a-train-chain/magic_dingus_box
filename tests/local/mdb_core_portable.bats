#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# mdb_core is the library the headless Mac engine (mdb_headless) links; it is
# EXCLUDE_FROM_ALL here and builds on Linux in test-local.yml's kiosk-build.
# A Linux build cannot catch a Linux-only header leaking into it — Linux has
# those headers — so this suite does: no source in MDB_CORE_SOURCES may reach,
# through its own or a project header's unconditional #include, a header that
# exists only on the Pi (DRM/GBM/EGL/GLES, evdev, gpiod, GStreamer, udev,
# systemd, linux/*). An include inside an #if block counts as guarded: the
# Mac build of mdb_core is what proves the guard holds.

setup() { cd "$TESTS_REPO_ROOT/magic_dingus_box_cpp"; }

scan() {
    python3 - "$@" <<'PY'
import os, re, sys
cmake = re.sub(r"#[^\n]*", "", open("CMakeLists.txt", encoding="utf-8").read())   # comments name files too

def cmake_list(name):
    m = re.search(r"set\(\s*" + name + r"\b(.*?)\n\s*\)", cmake, re.S)
    return re.findall(r"\S+", m.group(1)) if m else []

def expand(items, seen=()):
    out = []
    for it in items:
        v = re.fullmatch(r"\$\{(\w+)\}", it)
        if v:
            if v.group(1) not in seen:
                out += expand(cmake_list(v.group(1)), seen + (v.group(1),))
        elif it.endswith((".cpp", ".c")):
            out.append(it)
    return out

core = cmake_list("MDB_CORE_SOURCES")
m = re.search(r"list\(APPEND MDB_CORE_SOURCES(.*?)\)", cmake, re.S)
core += re.findall(r"\S+", m.group(1)) if m else []
sources = sorted(set(expand(core)))
if len(sources) < 20:
    sys.exit(f"parsed only {len(sources)} MDB_CORE_SOURCES files; the CMake layout changed")

FORBIDDEN = re.compile(r"^(xf86drm|gbm\.h|EGL/|GLES\d*/|libevdev|gpiod|gst/|libudev|systemd/|linux/|sys/inotify|sys/epoll)")
INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

def unconditional_includes(path):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    depth, out, guard = 0, [], None
    directives = [l.strip() for l in lines if l.strip().startswith("#")]
    if len(directives) > 1 and directives[0].startswith("#ifndef") and directives[1].startswith("#define"):
        guard = directives[0]   # a classic include guard is not a condition
    for l in lines:
        s = l.strip()
        if s == guard:
            guard = "used"
            continue
        if re.match(r"#\s*if", s):
            depth += 1
        elif re.match(r"#\s*endif", s):
            depth = max(0, depth - 1)
        elif depth == 0:
            mm = INC.match(l)
            if mm:
                out.append((mm.group(1), mm.group(2)))
    return out

bad, seen = [], set()
def walk(path, chain):
    if path in seen:
        return
    seen.add(path)
    for kind, name in unconditional_includes(path):
        if kind == "<":
            if FORBIDDEN.match(name):
                bad.append(" -> ".join(chain + [f"<{name}>"]))
            continue
        for base in (os.path.dirname(path), "src"):
            cand = os.path.normpath(os.path.join(base, name))
            if os.path.isfile(cand):
                walk(cand, chain + [cand])
                break

for src in sources:
    walk(src, [src])
for b in bad:
    print(b)
print(f"scanned {len(sources)} sources, {len(seen)} files", file=sys.stderr)
sys.exit(1 if bad else 0)
PY
}

@test "no mdb_core source reaches a Linux-only header unconditionally" {
    run scan
    echo "$output"
    [ "$status" -eq 0 ]
}
