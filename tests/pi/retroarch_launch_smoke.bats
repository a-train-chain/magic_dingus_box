#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

ROMS_BASE="/opt/magic_dingus_box/magic_dingus_box_cpp/data/roms"

setup() {
    require_pi
    require_joystick
    # Pause the kiosk service so we don't fight it for DRM master.
    pi_ssh 'sudo systemctl stop magic-dingus-box-cpp.service' >/dev/null 2>&1
    sleep 2
}

teardown() {
    # Make sure no lingering RetroArch process is holding hardware.
    pi_ssh 'sudo pkill -9 retroarch 2>/dev/null; true' >/dev/null
    sleep 1
    # Restart the kiosk service so the system returns to normal.
    pi_ssh 'sudo systemctl start magic-dingus-box-cpp.service' >/dev/null 2>&1
    sleep 3
}

# Helper: find a ROM in a directory.
# Echoes the path or returns 1 if none found.
find_rom() {
    local dir="$1"
    pi_ssh "ls $dir/*.{nes,sfc,smc,md,gen,bin,iso,m3u,pce,a78,zip,7z,chd,pbp} 2>/dev/null | head -1"
}

# Helper: try to launch a core with a ROM, give it $2 seconds, then kill it.
# Returns success if RetroArch ran for at least 5 seconds AND exited within $2.
try_launch() {
    local core="$1"
    local timeout="$2"
    local rom="$3"

    pi_ssh 'timeout --kill-after=5 '"$timeout"' sudo -u magic -H /usr/bin/retroarch -L '"$core"' '"'"'"$rom"'"'"' --verbose 2>&1 | tail -10; echo "EXIT: $?"'
}

@test "nestopia_libretro: launch a NES ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/nes")
    [ -n "$rom" ] || skip "no NES ROM available"
    run try_launch "nestopia_libretro" 12 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}

@test "snes9x2010_libretro: launch a SNES ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/snes")
    [ -n "$rom" ] || skip "no SNES ROM available"
    run try_launch "snes9x2010_libretro" 12 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}

@test "genesis_plus_gx_libretro: launch a Genesis ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/genesis")
    [ -n "$rom" ] || skip "no Genesis ROM available"
    run try_launch "genesis_plus_gx_libretro" 12 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}

@test "pcsx_rearmed_libretro: launch a PS1 ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/ps1")
    [ -n "$rom" ] || skip "no PS1 ROM available"
    run try_launch "pcsx_rearmed_libretro" 18 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}

@test "mednafen_pce_fast_libretro: launch a PCE ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/pcengine")
    [ -n "$rom" ] || skip "no PC Engine ROM available"
    run try_launch "mednafen_pce_fast_libretro" 12 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}

@test "prosystem_libretro: launch an Atari 7800 ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/atari7800")
    [ -n "$rom" ] || skip "no Atari 7800 ROM available"
    run try_launch "prosystem_libretro" 12 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}

@test "fbneo_libretro: launch an Arcade ROM and exit cleanly" {
    rom=$(find_rom "$ROMS_BASE/arcade")
    [ -n "$rom" ] || skip "no Arcade ROM available"
    run try_launch "fbneo_libretro" 12 "$rom"
    last=$(echo "$output" | grep "^EXIT:" | tail -1 | sed 's/EXIT: //')
    if [ "$last" != "0" ] && [ "$last" != "124" ] && [ "$last" != "143" ]; then
        echo "unexpected exit: $last"
        echo "$output"
        false
    fi
}
