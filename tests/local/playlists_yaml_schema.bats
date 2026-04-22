#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

PLAYLISTS_DIR="$CPP_DIR/data/playlists"

@test "playlists directory exists" {
    [ -d "$PLAYLISTS_DIR" ]
}

@test "every playlist file is valid YAML" {
    shopt -s nullglob
    yml_files=("$PLAYLISTS_DIR"/*.yml "$PLAYLISTS_DIR"/*.yaml)
    shopt -u nullglob
    [ ${#yml_files[@]} -gt 0 ]

    for f in "${yml_files[@]}"; do
        python3 -c "import yaml,sys; yaml.safe_load(open('$f'))" \
            || { echo "invalid YAML: $f"; false; }
    done
}

@test "every emulated_game item has core and path" {
    shopt -s nullglob
    for f in "$PLAYLISTS_DIR"/*.yml "$PLAYLISTS_DIR"/*.yaml; do
        python3 - "$f" <<'PY'
import sys, yaml
path = sys.argv[1]
data = yaml.safe_load(open(path))
items = (data or {}).get("items", [])
for i, item in enumerate(items):
    if item.get("source_type") == "emulated_game":
        for key in ("emulator_core", "path"):
            if not item.get(key):
                print(f"{path}: item {i} missing {key}")
                sys.exit(1)
PY
    done
    shopt -u nullglob
}

@test "every video item has a path" {
    shopt -s nullglob
    for f in "$PLAYLISTS_DIR"/*.yml "$PLAYLISTS_DIR"/*.yaml; do
        python3 - "$f" <<'PY'
import sys, yaml
path = sys.argv[1]
data = yaml.safe_load(open(path))
items = (data or {}).get("items", [])
for i, item in enumerate(items):
    stype = item.get("source_type", "video")
    if stype in ("video", "local") and not item.get("path"):
        print(f"{path}: item {i} (video) missing path")
        sys.exit(1)
PY
    done
    shopt -u nullglob
}
