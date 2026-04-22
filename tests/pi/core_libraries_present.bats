#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

CORE_DIR="/usr/lib/aarch64-linux-gnu/libretro"
EXPECTED_CORES=(
    "nestopia_libretro.so"
    "snes9x2010_libretro.so"
    "genesis_plus_gx_libretro.so"
    "pcsx_rearmed_libretro.so"
    "mednafen_pce_fast_libretro.so"
    "prosystem_libretro.so"
    "fbneo_libretro.so"
)

setup() { require_pi; }

@test "every expected libretro core is installed" {
    for core in "${EXPECTED_CORES[@]}"; do
        run pi_ssh "test -f '$CORE_DIR/$core'"
        [ "$status" -eq 0 ] || { echo "MISSING CORE: $CORE_DIR/$core"; false; }
    done
}

@test "PS1 BIOS (scph5501.bin) is present" {
    run pi_ssh 'test -f /home/magic/.config/retroarch/system/scph5501.bin'
    [ "$status" -eq 0 ]
}
