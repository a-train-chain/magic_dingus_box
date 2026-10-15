#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# One golden image runs on Pi 4B and Pi 5 (CLAUDE.md "Dual-board
# contract"): board differences resolve at RUNTIME through
# platform::PlatformProfile, never through hardcoded per-board values.
# Each check below is a grep that must find nothing; each pattern is a
# class of bug that has shipped before (Pi 4 sink names silencing a Pi 5,
# /dev/gpiochip0 being the wrong chip on a Pi 5, ...).

setup() { cd "$TESTS_REPO_ROOT"; }

@test "no hardcoded /dev/gpiochipN in kiosk sources" {
    run git grep -nE '"/dev/gpiochip[0-9]' -- magic_dingus_box_cpp/src
    [ "$status" -ne 0 ] || { echo "$output"; false; }
}

@test "no hardcoded SoC audio sink addresses outside tests and the resolver" {
    run git grep -nE 'platform-(fef00700|fe00b840|107c70[0-9a-f]+)\.' -- . \
        ':!magic_dingus_box_cpp/tests' ':!tests' \
        ':!magic_dingus_box_cpp/scripts/resolve_audio_sink.sh' ':!*.md'
    [ "$status" -ne 0 ] || { echo "$output"; false; }
}

@test "no compile-time board #ifdefs in kiosk sources" {
    run git grep -nEi '^\s*#\s*if(n)?def.*(PI4|PI5|RPI|RASPBERRY|BCM27|BCM2712)' -- magic_dingus_box_cpp/src
    [ "$status" -ne 0 ] || { echo "$output"; false; }
}

@test "only PlatformProfile reads /proc/device-tree/model" {
    run git grep -lF '/proc/device-tree/model' -- 'magic_dingus_box_cpp/src/*.cpp' 'magic_dingus_box_cpp/src/*.h'
    for f in $output; do
        [[ "$f" == magic_dingus_box_cpp/src/platform/platform_profile.* ]] || { echo "board detection outside PlatformProfile: $f"; false; }
    done
}

@test "boot-config reference keeps model-specific settings out of [all]" {
    run awk '
        /^\[/ { section = $0; next }
        /^(v3d_freq|kernel|gpu_mem)=/ && section !~ /^\[pi[45]\]/ { print NR ": " $0 " in " (section == "" ? "(top)" : section) }
    ' magic_dingus_box_cpp/scripts/data/boot-config.reference.txt
    [ -z "$output" ] || { echo "$output"; false; }
}
