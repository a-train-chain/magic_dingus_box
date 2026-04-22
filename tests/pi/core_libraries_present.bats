#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Cores can live in either system or user directory — the kiosk's
# retroarch_launcher.cpp checks both locations at launch time.
SYSTEM_CORE_DIR="/usr/lib/aarch64-linux-gnu/libretro"
USER_CORE_DIR="/home/magic/.config/retroarch/cores"
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

@test "every expected libretro core is installed (system or user dir)" {
    for core in "${EXPECTED_CORES[@]}"; do
        run pi_ssh "test -f '$SYSTEM_CORE_DIR/$core' || test -f '$USER_CORE_DIR/$core'"
        [ "$status" -eq 0 ] \
            || { echo "MISSING CORE: $core (looked in $SYSTEM_CORE_DIR/ and $USER_CORE_DIR/)"; false; }
    done
}

@test "PS1 BIOS (scph5501.bin) is present" {
    run pi_ssh 'test -f /home/magic/.config/retroarch/system/scph5501.bin'
    [ "$status" -eq 0 ]
}
