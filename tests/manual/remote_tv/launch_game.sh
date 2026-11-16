#!/bin/bash
# launch_game.sh DOWNS (needs PI_HOST): opens the guide, Settings, Video Games,
# clamps to the first library (Arcade), steps DOWN x DOWNS, opens the list and
# launches its highlighted ROM, then reports which core RetroArch runs and
# which HDMI PCM it holds (RUNNING, owned by RetroArch). End the game with after_game.sh on the box.
T="$(dirname "$0")/tv.sh"; downs=$1
bash "$T" p tap OK; sleep 1.3
bash "$T" p tap BLACK; sleep 1.5
bash "$T" p tap OK; sleep 1.8
bash "$T" p tap UP 11 0.9; sleep 0.8
[ "$downs" -gt 0 ] && { bash "$T" p tap DOWN "$downs" 1.0; sleep 0.8; }
bash "$T" p tap OK; sleep 2.0
bash "$T" p tap OK
for i in $(seq 1 50); do sleep 0.5; ssh -o BatchMode=yes "${PI_HOST:?set PI_HOST}" 'pgrep -x retroarch >/dev/null' && break; done; sleep 5
ssh -o BatchMode=yes "${PI_HOST:?set PI_HOST}" 'c=$(pgrep -a retroarch | grep -o "\-L [a-z0-9_]*" | head -1); r=$(pgrep -a retroarch | grep -o "[^/]*\.[a-z0-9]*$" | head -1); echo "game: ${c:-NONE} ${r}"; for st in /proc/asound/card*/pcm0p/sub0/status; do echo "  $(echo $st | cut -d/ -f4): $(grep -E "^state|^owner_pid" $st 2>/dev/null | tr "\n" " ")"; done'
