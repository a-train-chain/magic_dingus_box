#!/bin/bash
# On the box (as magic): end RetroArch like a normal quit (the kiosk treats
# SIGTERM as one), wait for the kiosk, then report screen/overlay/channel and
# the audio state of every HDMI card.
D=/opt/magic_dingus_box/magic_dingus_box_cpp/data
sudo pkill -TERM -x retroarch
for i in $(seq 1 60); do
  sleep 0.5
  pgrep -x retroarch >/dev/null && continue
  read -r scr ov age name title <<< "$(python3 -c "
import json,time
s=json.load(open('$D/kiosk_status.json'))
print(s['screen'], s['overlay_visible'], round(time.time()-s['ts'],1), (s['playlist'] or {}).get('name','').replace(' ','_'), (s['now_playing'] or {}).get('title','').replace(' ','_'))")"
  case "$scr" in retroarch|"") continue;; esac
  [ "${age%.*}" -lt 2 ] && break
done
sleep 4
python3 -c "
import json,time
s=json.load(open('$D/kiosk_status.json'))
print('back: screen=%s overlay=%s channel=%s item=%s pos=%.1f' % (s['screen'], s['overlay_visible'], (s['playlist'] or {}).get('name'), (s['now_playing'] or {}).get('title'), (s.get('playback') or {}).get('position_sec') or 0))"
export XDG_RUNTIME_DIR=/run/user/$(id -u)
echo "sinks: $(pactl list short sinks | awk '{print $2, $NF}' | tr '\n' ' ')"
for st in /proc/asound/card*/pcm0p/sub0/status; do
  echo "$(echo "$st" | cut -d/ -f4): $(grep -E '^state|^owner_pid' "$st" 2>/dev/null | tr '\n' ' ')"
done
echo "pulseaudio pid: $(pgrep -x pulseaudio)"
