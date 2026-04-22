#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

LAUNCHER="/home/magic/retroarch_launcher.sh"

setup() {
    require_pi
    # Need a launcher script from a recent game launch.
    pi_ssh "test -f $LAUNCHER" || skip "no $LAUNCHER on Pi (launch a game once first)"
}

# Extract the heredoc body (the config that gets written to $UI_CONFIG)
extract_config() {
    pi_ssh 'awk '"'"'/^cat > "\$UI_CONFIG" << .EOF./,/^EOF$/ {print}'"'"' '"$LAUNCHER"' | sed '"'"'1d;$d'"'"
}

@test "launcher script has a UI_CONFIG heredoc" {
    extract=$(extract_config)
    [ -n "$extract" ] || { echo "no heredoc found"; false; }
}

@test "launcher emits required video keys" {
    config=$(extract_config)
    for key in video_driver video_fullscreen video_aspect_ratio aspect_ratio_index input_player1_a_btn input_player1_b_btn; do
        echo "$config" | grep -q "^$key" || { echo "missing key: $key"; false; }
    done
}

@test "launcher emits a sensible video_fullscreen_x value" {
    config=$(extract_config)
    val=$(echo "$config" | grep '^video_fullscreen_x' | head -1 | sed 's/.*= *"//; s/".*//')
    case "$val" in
        640|1920) : ;;
        *) echo "unexpected video_fullscreen_x: $val"; false ;;
    esac
}

@test "launcher pins aspect to 1.333 (4:3)" {
    config=$(extract_config)
    val=$(echo "$config" | grep '^video_aspect_ratio' | head -1 | sed 's/.*= *"//; s/".*//')
    [ "$val" = "1.333" ]
}

@test "if Modern TV mode, custom_viewport is at expected (251, 10, 1415, 1059)" {
    config=$(extract_config)
    enabled=$(echo "$config" | grep '^video_custom_viewport_enable' | head -1 | sed 's/.*= *"//; s/".*//')
    if [ "$enabled" != "true" ]; then
        skip "CRT mode (custom_viewport disabled)"
    fi
    x=$(echo "$config" | grep '^video_custom_viewport_x ' | head -1 | sed 's/.*= *"//; s/".*//')
    y=$(echo "$config" | grep '^video_custom_viewport_y ' | head -1 | sed 's/.*= *"//; s/".*//')
    w=$(echo "$config" | grep '^video_custom_viewport_width' | head -1 | sed 's/.*= *"//; s/".*//')
    h=$(echo "$config" | grep '^video_custom_viewport_height' | head -1 | sed 's/.*= *"//; s/".*//')
    [ "$x" = "251" ] && [ "$y" = "10" ] && [ "$w" = "1415" ] && [ "$h" = "1059" ] || { echo "viewport mismatch: ($x, $y, $w, $h) want (251, 10, 1415, 1059)"; false; }
}
