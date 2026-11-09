# Bluetooth controller pairing + automatic layouts — design

2026-11-08. Owner's brief: "the easiest process possible for someone to get
a Magic Dingus Box, go to pair controller, and have it walk them through"
— built into the existing Controller Setup screen — plus "the best layout
for the popular controllers people already have".

## What the customer does

1. Settings → **Pair Wireless Controller** (or Controller Setup → Green).
   The box's knob and buttons drive it, because the customer may have no
   working controller yet.
2. The screen shows how to put each common pad in pairing mode
   (PS5: Create + PS; PS4: Share + PS; Xbox: pair button; Switch Pro: sync
   button; 8BitDo: Start + Y, then pair).
3. The box finds the pad, pairs it, trusts it and connects it. There is no
   list to choose from.
4. "Connected! Press any button on it." That press confirms the pad works
   and tells the box which input device it is.
5. PlayStation / Xbox / Switch Pro / 8BitDo → a "try the buttons" screen,
   then **Ready to play!** with no mapping. Any other pad → the existing
   mapping steps, with the pad already chosen.

From then on the pad reconnects whenever its PS / Home / Xbox button is
pressed (it is trusted in BlueZ).

## Layouts: everything popular is PS1-shaped

PS4/PS5, Xbox, Switch Pro and 8BitDo all have the DualShock's shape: a
diamond of four face buttons, L1/R1 + L2/R2, Select/Start, two clickable
sticks and a d-pad. So they all use the **existing PS-style semantic
tables**, matched by **position**. The bottom face button is "Cross" whether
it is printed A (Xbox) or B (Nintendo), which is also RetroArch's own
convention. A SNES game on a Switch Pro pad then gets B at the bottom and A
on the right, as the SNES had.

The per-pad question is only which evdev code each position reports. The
KERNEL DRIVER decides that, not the pad:

| driver | top / left face codes | L2 / R2 | right stick |
|---|---|---|---|
| playstation, sony, nintendo | 0x133 / 0x134 (positional) | BTN_TL2/TR2 | RX/RY |
| xpad, xpadneo | 0x134 / 0x133 (Xbox X = BTN_X = left) | ABS_Z / ABS_RZ | RX/RY |
| microsoft, or VID 045e (Xbox over BLE) | 0x134 / 0x133 | ABS_BRAKE / ABS_GAS | Z/RZ |
| hid-generic, anything else | — no automatic layout — | | |

The tokens were cross-checked against RetroArch's own udev autoconfig for
the DualSense and the Xbox 360 pad. Analog triggers needed one mapping-layer
addition: a RetroPad button slot assigned to an axis is emitted as
`input_playerN_<slot>_axis` (that is how an Xbox trigger reaches N64 Z).
Resolution order is unchanged except for one new step: captured → built-in
→ **automatic** → legacy N64. A pad's Controller Setup capture always wins,
and automatic layouts are never written to the profile store, so they
improve with updates.

## Decisions (builder's calls, made with the owner's go-ahead to proceed)

- **No device list.** The flow picks the strongest-signal *gamepad* that is
  advertising: class, icon or appearance, and the name only when BlueZ knows
  nothing else. It never picks phones, headsets or keyboards.
- **Stale pairings are removed first.** A pad back in pairing mode has
  dropped its keys (for example after being re-paired to a PS5), while
  BlueZ still holds the old ones.
- **Stop discovery before pairing.** The Pi's Bluetooth shares one combo
  chip with Wi-Fi on both boards.
- **One retry**, then a "Couldn't pair" screen where Red retries.
- **The BlueZ session exists only while the pairing screen is up**: one
  detached thread, its own D-Bus connection, and a NoInputNoOutput default
  agent (PIN "0000" for legacy pads). `stop()` never joins, so a wedged
  `bluetoothd` cannot hold the render loop past the 10 s systemd watchdog.
- **Board parity.** The Pi 4B and Pi 5 have the same on-board radio, so
  there is no platform gate and no `PlatformProfile` field.
- **Permissions.** Nothing special on Trixie: the box's BlueZ D-Bus policy
  (bluez 5.82) lets any local user call `org.bluez`, checked on a Pi 5.
  `setup_bluetooth.sh` (via `setup_memory_tuning.sh` step 1h) enables
  `bluetooth.service`, unblocks the radio, and adds the kiosk user to the
  `bluetooth` group as a fallback for older policies that required it.
- **Golden image.** `/var/lib/bluetooth` (link keys) is stashed out of the
  image with `bluetooth.service` stopped, restored on the source box, and
  wiped by `first_boot.sh`.
- **Found along the way.** A DualSense's motion-sensor and touchpad input
  devices both report ABS_X and were being opened as extra pads, so tilting
  the controller scrolled the menu. The kiosk now applies the kernel joydev
  blacklist (touch, digitizer, accelerometer).

## Not done / not verified

- **Nothing has run on real hardware with a real controller.** The pairing
  flow, layouts and screen logic are unit-tested on the Mac, and the kiosk
  binary builds for Debian/arm64. Before a release, pair a DualSense, an
  Xbox Series pad and a Switch Pro (or 8BitDo in Switch mode) on a Pi 5 AND
  a Pi 4B. On each, check menu navigation, a PS1 game, an N64 game (Z on L2)
  and the exit gesture. The Xbox-over-BLE and Switch Pro rows above follow
  the kernel drivers' documented mappings and are the most likely to need a
  correction.
- What HAS run off-hardware (2026-11-08): the real `BluezWorker` +
  `PairingFlow`, pumped like the wizard, against a stand-in `org.bluez`
  service on a Debian Trixie system bus. That run covered power on,
  discovery, picking the DualSense, stopping discovery, and pairing, where
  the fake asks the kiosk's agent for a PIN mid-call and gets "0000". It
  then trusted and connected the pad, read IDs 054c:0ce6 from the modalias,
  and unregistered the agent on stop. Against a system bus with no BlueZ at
  all, the flow lands on "Bluetooth isn't available" and `stop()` returns
  at once.
- No "Forget Bluetooth controllers" row: re-pairing already replaces a stale
  pairing. A Settings → System row would be a small follow-up.
- No `verify_box.sh` Bluetooth check yet.
- Two identical pads (two DualSenses) share a VID:PID, so RetroArch's port
  reservation cannot tell them apart. This was already true for identical
  USB pads.
