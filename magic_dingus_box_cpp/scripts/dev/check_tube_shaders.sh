#!/bin/bash
# check_tube_shaders.sh <dir> - run glslangValidator on every assembled Tube
# program in <dir>, as written by
#   MDB_WRITE_TUBE_GLSL=<dir> ./build/test_app_unit "[tube_shaders]"
# (the very sources the kiosk embeds and compiles; tube_shader_assembly.h).
# Each *.frag is checked as a fragment shader, fullscreen.vert as a vertex shader,
# both as GLSL ES 3.00 (their own version line).
#
# Prints "N checked, M failed" and exits 1 on any failure, or when N is not the
# table's count: 19 programs with hardware scene decode + the 9 that read the
# scene again with shader decode + the vertex shader = 29. A count that drifts
# means the table or the writer changed without this script.
#
# CI: the tube-shaders job in .github/workflows/test-local.yml.
set -u

EXPECTED=29

dir="${1:-}"
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
    echo "usage: $0 <dir of assembled .frag/.vert files>" >&2
    exit 2
fi
if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator not found (apt: glslang-tools, brew: glslang)" >&2
    exit 2
fi

n=0
failed=0
check() {
    local stage=$1 file=$2 out
    n=$((n + 1))
    if ! out="$(glslangValidator -S "$stage" "$file" 2>&1)"; then
        failed=$((failed + 1))
        echo "FAIL $(basename "$file")"
        echo "$out" | head -20
    fi
}

shopt -s nullglob
for f in "$dir"/*.frag; do
    check frag "$f"
done
if [ -f "$dir/fullscreen.vert" ]; then
    check vert "$dir/fullscreen.vert"
fi

echo "$n checked, $failed failed"
if [ "$n" -ne "$EXPECTED" ]; then
    echo "expected $EXPECTED programs, found $n" >&2
    exit 1
fi
[ "$failed" -eq 0 ]
