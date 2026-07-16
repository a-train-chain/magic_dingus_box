#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

@test "latest RetroArch handoff has no page flips after DRM release" {
    run pi_ssh 'sudo journalctl -u magic-dingus-box-cpp.service --since "5 minutes ago" --no-pager 2>/dev/null'
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -F 'Failed to set CRTC: Permission denied'
}

@test "latest RetroArch output has no fatal display signature" {
    run pi_ssh 'python3 - <<'"'"'PY'"'"'
from pathlib import Path

path = Path("/home/magic/retroarch_launcher.log")
log = path.read_text(errors="replace") if path.exists() else ""
latest = log.rsplit("Launcher: Launching RetroArch directly...", 1)[-1]
fatal = [
    signature
    for signature in ("Failed to connect to Wayland server", "QueuePresent failed")
    if signature in latest
]
raise SystemExit("fatal video signatures: " + ", ".join(fatal) if fatal else 0)
PY'
    [ "$status" -eq 0 ]
}
