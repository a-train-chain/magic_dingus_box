# Bluetooth game input and menu repeat fixes

The owner approved fixing both diagnosed issues and deploying/testing on the MDB, using Sol 6.1 for implementation and tests and Astra only when needed.

The connected Switch Pro-compatible pad (057e:2009, nintendo driver) appears as event7, with its IMU as event8, and no js node. The real launcher detects zero pads because it scans only js nodes. RetroArch sees the phone remote first and the pad second, while the generated configuration uses the legacy N64 adapter layout and no reservation. The kiosk correctly recognizes the pad through evdev. Vertical analog navigation emits once per report beyond 16000, whereas the D-pad has a 400 ms initial repeat delay.

## Approach

Use input event devices for launcher discovery, reusing the existing capability reader and controller classification. Exclude the phone remote and auxiliary motion/touch devices. Preserve captured/builtin/automatic mapping precedence and physical controller reservations. A legacy-js workaround would retain the faulty assumption and is unnecessary because RetroArch uses event devices.

Use a small portable direction/repeat state machine for menu sticks, with normalized ranges and hysteresis, firing once on a deliberate direction, then repeating after 400 ms at the existing D-pad cadence. Keep state per device and independent of D-pad holds. Reset it on neutral, removal, capture-mode changes and cleanup. Do not change RetroArch gameplay axes or the established D-pad behavior.

## Constraints and acceptance

- One runtime implementation and one binary support Pi 4B and Pi 5 on Debian Trixie; no board compile switches or new system dependencies.
- Keep N64/Dreamcast gated off on Pi 4; exercise a supported core there if hardware is reachable.
- Tests must cover event-only Bluetooth pads, phone-remote exclusion, auxiliary nodes, mixed/identical wired pads, numeric ordering, and disappearance during scanning.
- Menu tests must cover dense input bursts, stable holds without new events, neutral/reversal, threshold jitter, signed/unsigned ranges, multiple devices, disconnect and capture reset.
- Run all available Mac suites and build the Linux kiosk; use the real Pi 5 to verify discovery, generated mapping/reservations, game launch and return, and menu repetition. Report physical-button checks separately from synthetic input checks.
- Preserve unrelated changes, content, saves and settings. Deployment is authorized. Do not publish a release or push unrelated work.
