#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# install_cores.sh runs under set -euo pipefail, and its download loop's
# failure branch used to be dead code: a bare `wget` that failed killed
# the whole script mid-loop, so one transient network error left every
# remaining core uninstalled — worst from update.sh's OTA cores
# bootstrap (`bash install_cores.sh || log_warn`), which then continued
# with games that cannot launch. --pi mode masked the bug on the bench:
# typeset -f ships the install function WITHOUT the set -e prologue.
# This runs the REAL script in local mode with stubbed sudo/wget/unzip/
# getent and proves the loop survives a failed download.

setup_file() {
    # The script uses associative arrays (declare -A) — macOS /bin/bash
    # is 3.2. Runs on Linux CI and on the Pi; skips on a stock Mac.
    bash -c 'declare -A _t=()' 2>/dev/null \
        || skip "needs bash 4+ (associative arrays)"
}

@test "a failed core download does not abort the remaining installs" {
    tmp="$BATS_TEST_TMPDIR/cores_run"
    mkdir -p "$tmp/scripts" "$tmp/bin" "$tmp/home"
    cp "$CPP_DIR/scripts/install_cores.sh" "$tmp/scripts/"

    # sudo: swallow apt entirely (no network, no root); run anything else.
    cat > "$tmp/bin/sudo" <<'EOF'
#!/usr/bin/env bash
[ "$1" = "apt" ] && exit 0
exec "$@"
EOF
    # getent: answer with the overridden $HOME so CORE_DIR stays inside
    # the sandbox instead of the invoking user's real home.
    cat > "$tmp/bin/getent" <<'EOF'
#!/usr/bin/env bash
printf 'u:x:1:1:g:%s:/bin/bash\n' "$HOME"
EOF
    # wget: fail only for the fbneo zip; create the -O target otherwise
    # (a real failed `wget -O` also leaves a zero-byte file behind).
    cat > "$tmp/bin/wget" <<'EOF'
#!/usr/bin/env bash
out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in
        -O) out="$2"; shift 2 ;;
        -*) shift ;;
        *)  url="$1"; shift ;;
    esac
done
case "$url" in
    *fbneo*) : > "$out"; exit 4 ;;
esac
echo "zipdata" > "$out"
EOF
    # unzip -o -d DIR ZIP: "extract" by creating DIR/<zip minus .zip>.
    cat > "$tmp/bin/unzip" <<'EOF'
#!/usr/bin/env bash
dir=""; zip=""
while [ $# -gt 0 ]; do
    case "$1" in
        -d) dir="$2"; shift 2 ;;
        -*) shift ;;
        *)  zip="$1"; shift ;;
    esac
done
so="$(basename "$zip" .zip)"
mkdir -p "$dir"
echo "so" > "$dir/$so"
EOF
    chmod +x "$tmp/bin/"*

    HOME="$tmp/home" PATH="$tmp/bin:$PATH" SUDO_USER="" \
        run bash "$tmp/scripts/install_cores.sh"

    # The old code could NEVER reach the ✗ branch (set -e killed the
    # script at the failed bare wget) and never exited 0 on a failure.
    [ "$status" -eq 0 ]
    [[ "$output" == *"✗ Failed to download fbneo_libretro.so"* ]]

    # Every OTHER core must be installed in BOTH consumer locations —
    # regardless of where fbneo fell in the (unordered) iteration.
    for so in nestopia_libretro.so pcsx_rearmed_libretro.so \
              genesis_plus_gx_libretro.so snes9x2010_libretro.so \
              mednafen_pce_fast_libretro.so prosystem_libretro.so \
              mupen64plus_next_libretro.so parallel_n64_libretro.so \
              flycast_libretro.so; do
        [ -f "$tmp/home/.config/retroarch/cores/$so" ]
        [ -f "$tmp/libretro_cores/$so" ]
    done
    [ ! -f "$tmp/home/.config/retroarch/cores/fbneo_libretro.so" ]

    # The failed download's temp zip must not linger.
    [ ! -f /tmp/fbneo_libretro.so.zip ]
}
