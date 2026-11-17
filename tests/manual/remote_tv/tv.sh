#!/bin/bash
# Mac-side helper for attended checklist runs on a real box (see README.md).
# PI_HOST must name the box. Usage:
#   tv.sh start                 # stage + start the persistent gamepad (root, transient unit)
#   tv.sh end                   # stop it and remove the staged files
#   tv.sh p tap DOWN 3          # send a press command
#   tv.sh st                    # one-line kiosk state
#   tv.sh shot NAME             # capture the next drawn frame -> shots/NAME.png
set -uo pipefail
HOST="${PI_HOST:?set PI_HOST=magic@<box ip> (never defaulted: two boxes are usually reachable)}"
DATA=/opt/magic_dingus_box/magic_dingus_box_cpp/data
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/shots"
ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" "$@"; }

case "${1:-}" in
start)
    scp -q "$HERE/mdbdrive.py" "$HOST:/tmp/mdbdrive.py" || exit 1
    ssh_ 'sudo install -d -m 0700 /run/mdb-drive && sudo install -o root -g root -m 0700 /tmp/mdbdrive.py /run/mdb-drive/mdbdrive.py && rm -f /tmp/mdbdrive.py &&
          sudo systemd-run --quiet --unit mdb-drive --property=RuntimeMaxSec=6h /usr/bin/python3 /run/mdb-drive/mdbdrive.py serve &&
          sleep 3 && sudo journalctl -u mdb-drive --no-pager -n 2 -o cat'
    ;;
end)
    ssh_ 'sudo python3 /run/mdb-drive/mdbdrive.py stop 2>/dev/null; sleep 1; sudo systemctl stop mdb-drive 2>/dev/null; sudo systemctl reset-failed mdb-drive 2>/dev/null; sudo rm -rf /run/mdb-drive; systemctl list-units --all --no-legend "mdb-drive*" | wc -l'
    ;;
p)
    shift; ssh_ "sudo timeout 30 python3 /run/mdb-drive/mdbdrive.py $*"
    ;;
st)
    ssh_ "python3 - <<'EOF'
import json, time
s = json.load(open('$DATA/kiosk_status.json'))
age = time.time() - s.get('ts', 0)
pick = {k: s.get(k) for k in ('screen', 'overlay_visible', 'playlist', 'now_playing')}
st = s.get('settings') or {}
pick['settings'] = {k: st.get(k) for k in ('active', 'highlighted_label')} if st else None
pb = s.get('playback') or {}
pick['pos'] = pb.get('position_sec')
print('age=%.1fs' % age, json.dumps(pick, default=str)[:600])
EOF"
    ;;
shot)
    name="$2"
    ssh_ "set -e; cd $DATA; before=\$(ls screenshots 2>/dev/null | sort | tail -1); touch screenshot_request; for i in \$(seq 1 40); do sleep 0.25; now=\$(ls screenshots 2>/dev/null | sort | tail -1); [ \"\$now\" != \"\$before\" ] && [ ! -e screenshot_request ] && break; done; [ \"\$now\" != \"\$before\" ] || { echo 'no new frame'; exit 1; }; sleep 0.5; echo \$now" > "$HERE/.last" || { cat "$HERE/.last"; exit 1; }
    f=$(tail -1 "$HERE/.last")
    scp -q "$HOST:$DATA/screenshots/$f" "$HERE/shots/$name.bmp" && ssh_ "rm -f $DATA/screenshots/$f" \
      && sips -s format png "$HERE/shots/$name.bmp" --out "$HERE/shots/$name.png" >/dev/null && rm -f "$HERE/shots/$name.bmp" \
      && echo "$HERE/shots/$name.png"
    ;;
*) echo "usage: tv.sh start|end|p ...|st|shot NAME" >&2; exit 2 ;;
esac
