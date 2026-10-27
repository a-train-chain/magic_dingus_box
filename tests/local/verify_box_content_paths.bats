#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh's "Content" check counts playlist paths that resolve on
# disk. The playlists are written by PyYAML, which escapes an apostrophe in
# a single-quoted scalar as '' — a regex reader kept the doubled quote
# literally, found no such file, and FAILed a good Pi 4B (2026-10-26:
# "video playlists: N-1/N present" for a video the kiosk played fine).
# verify_box.sh runs top-to-bottom against live hardware, so the check's
# inline Python is extracted and run against a fixture app tree.

setup() {
    PY="$BATS_TEST_TMPDIR/content_check.py"
    sed -n "/^done < <(python3 - \"\$APP\" <<'PY'\$/,/^PY\$/p" \
        "$CPP_DIR/scripts/verify_box.sh" | sed '1d;$d' > "$PY"
    [ -s "$PY" ] || { echo "Content check python not found in verify_box.sh"; return 1; }
    APP="$BATS_TEST_TMPDIR/app"
    mkdir -p "$APP/data/playlists" "$APP/data/media" "$APP/data/roms/nes"
}

content_check() {
    python3 "$PY" "$APP"
}

@test "apostrophe in a single-quoted path counts as present" {
    touch "$APP/data/media/Disney's One Saturday Morning ｜ 2001 (cXku3KLXgZU).mp4"
    touch "$APP/data/media/Plain.mp4"
    cat > "$APP/data/playlists/Cartoons.yaml" <<'EOF'
title: Cartoons
items:
  - title: One Saturday Morning
    source_type: local
    path: 'media/Disney''s One Saturday Morning ｜ 2001 (cXku3KLXgZU).mp4'
  - title: Plain
    source_type: local
    path: media/Plain.mp4
EOF
    run content_check
    [ "$status" -eq 0 ]
    echo "$output"
    [[ "$output" == *"VID 2 2"* ]]
    [[ "$output" != *"BAD"* ]]
}

@test "double-quoted escapes and both resolution bases still resolve" {
    touch "$APP/data/roms/nes/Castlevania III - Dracula's Curse (USA).zip"
    touch "$APP/data/media/Say \"Hi\".mp4"
    cat > "$APP/data/playlists/games_nes.yaml" <<'EOF'
title: "NES Games"
items:
- title: "Castlevania III"
  source_type: emulated_game
  path: "data/roms/nes/Castlevania III - Dracula's Curse (USA).zip"
EOF
    cat > "$APP/data/playlists/Quotes.yaml" <<'EOF'
title: Quotes
items:
  - source_type: local
    path: "media/Say \"Hi\".mp4"
EOF
    run content_check
    [ "$status" -eq 0 ]
    echo "$output"
    [[ "$output" == *"GAME 1 1"* ]]
    [[ "$output" == *"VID 1 1"* ]]
    [[ "$output" != *"BAD"* ]]
}

@test "a genuinely missing file is still reported" {
    cat > "$APP/data/playlists/Cartoons.yaml" <<'EOF'
title: Cartoons
items:
  - source_type: local
    path: 'media/It''s Gone.mp4'
EOF
    run content_check
    [ "$status" -eq 0 ]
    echo "$output"
    [[ "$output" == *"VID 0 1"* ]]
    [[ "$output" == *"BAD Cartoons.yaml 1missing"* ]]
}

@test "without PyYAML the regex fallback unescapes quotes too" {
    mkdir -p "$BATS_TEST_TMPDIR/noyaml"
    echo 'raise ImportError("blocked for test")' > "$BATS_TEST_TMPDIR/noyaml/yaml.py"
    touch "$APP/data/media/Disney's Recess.mp4" "$APP/data/media/Say \"Hi\".mp4" "$APP/data/media/Plain.mp4"
    cat > "$APP/data/playlists/Cartoons.yaml" <<'YAML'
title: Cartoons
items:
  - source_type: local
    path: 'media/Disney''s Recess.mp4'
  - source_type: local
    path: "media/Say \"Hi\".mp4"
  - path: media/Plain.mp4
YAML
    run env PYTHONPATH="$BATS_TEST_TMPDIR/noyaml" python3 "$PY" "$APP"
    [ "$status" -eq 0 ]
    echo "$output"
    [[ "$output" == *"VID 3 3"* ]]
    [[ "$output" != *"BAD"* ]]
}
