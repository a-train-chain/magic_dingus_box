#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Verify every emulated_game item in every playlist on the Pi resolves
# to a real ROM file on disk. Catches: ROM renamed/deleted/moved but
# playlist not updated → game shows in kiosk menu, errors on launch
# on EVERY clone.

PI_PLAYLISTS_DIR="/opt/magic_dingus_box/magic_dingus_box_cpp/data/playlists"
# The kiosk treats playlist paths as relative to the cpp install root
# (which is the parent of data/). So a path like "data/roms/x/y.zip"
# resolves to "<install_root>/data/roms/x/y.zip", not "<data_dir>/data/...".
PI_INSTALL_ROOT="/opt/magic_dingus_box/magic_dingus_box_cpp"

setup() { require_pi; }

@test "playlists directory exists on Pi" {
    run pi_ssh "test -d $PI_PLAYLISTS_DIR"
    [ "$status" -eq 0 ]
}

@test "every emulated_game ROM path referenced by playlists exists on Pi" {
    # Iterate playlists, parse YAML with python3 (already on Pi for the kiosk),
    # extract every emulated_game.path, verify each file exists.
    # Paths are interpreted relative to the data dir.
    run pi_ssh "
        set -e
        cd $PI_INSTALL_ROOT
        missing=0
        for f in $PI_PLAYLISTS_DIR/*.yaml $PI_PLAYLISTS_DIR/*.yml; do
            [ -f \"\$f\" ] || continue
            python3 -c \"
import yaml, os, sys
with open('\$f') as fp:
    data = yaml.safe_load(fp)
items = (data or {}).get('items', [])
for i, item in enumerate(items):
    if item.get('source_type') == 'emulated_game':
        p = item.get('path', '')
        if not p:
            continue
        # Path may be relative to data dir, or absolute, or relative to playlist
        if os.path.isabs(p):
            full = p
        else:
            full = os.path.join('.', p) if not p.startswith('data/') else p
        if not os.path.isfile(full):
            print(f'MISSING: \$f item {i} ({item.get(\\\"title\\\", \\\"?\\\")}) -> {p}')
            sys.exit(1)
\" || { missing=1; }
        done
        exit \$missing
    "
    if [ "$status" -ne 0 ]; then
        echo "$output"
        false
    fi
}

@test "every video item path referenced by playlists exists on Pi (or is intentionally absent)" {
    # Video files live in data/media/. They may legitimately be absent
    # right after a fresh prep_golden_image (which clears media/), so
    # SKIP this test if the media directory is empty rather than fail.
    run pi_ssh "
        if [ ! -d $PI_INSTALL_ROOT/data/media ] || [ -z \"\$(ls -A $PI_INSTALL_ROOT/data/media 2>/dev/null)\" ]; then
            echo 'SKIP_REASON: data/media/ empty (post-prep golden state)'
            exit 77
        fi
        cd $PI_INSTALL_ROOT
        missing=0
        for f in $PI_PLAYLISTS_DIR/*.yaml $PI_PLAYLISTS_DIR/*.yml; do
            [ -f \"\$f\" ] || continue
            python3 -c \"
import yaml, os, sys
with open('\$f') as fp:
    data = yaml.safe_load(fp)
items = (data or {}).get('items', [])
for i, item in enumerate(items):
    stype = item.get('source_type', 'video')
    if stype in ('video', 'local'):
        p = item.get('path', '')
        if not p:
            continue
        if os.path.isabs(p):
            full = p
        else:
            full = p
        if not os.path.isfile(full):
            print(f'MISSING VIDEO: \$f item {i} ({item.get(\\\"title\\\", \\\"?\\\")}) -> {p}')
            sys.exit(1)
\" || { missing=1; }
        done
        exit \$missing
    "
    case "$status" in
        0) ;;
        77) skip "$output" ;;
        *) echo "$output"; false ;;
    esac
}
