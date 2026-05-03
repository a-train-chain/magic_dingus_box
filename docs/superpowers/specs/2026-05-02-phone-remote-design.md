# Phone Remote — Design

**Status:** Spec · **Date:** 2026-05-02 · **Owner:** Alexander Chaney

## Goal

A phone-based remote that lets a household member operate the Magic Dingus Box with one thumb at first-class quality: navigation feel comparable to the rotary encoder / USB controller, sub-80 ms p95 input-to-display latency on a typical home Wi-Fi network, and an aesthetic that mirrors the physical box's button semantics. Pairing is initiated from the kiosk Settings menu via QR code; once paired, a phone gets near-instant access on subsequent connections.

The remote is delivered as a tab inside the existing Content Manager web admin (Flask, port 5000) — *not* as a separate URL — so a phone-holder only ever needs to remember one address. On a phone-sized viewport the Remote tab takes over the full screen and feels like a native app; on desktop it stays inside the admin shell as a debug surface.

## Non-goals (v1)

- Phone as a gameplay gamepad inside RetroArch (only a "Quit Game" affordance is exposed during game sessions)
- Phone-side text input / virtual keyboard for kiosk search fields
- Cross-LAN / public-internet access (LAN only; no port forward, no TLS, no public auth)
- Voice control
- Push notifications (e.g., "download complete")
- Drag-to-scrub continuous seeking (tap-to-seek only in v1; possible v1.1 polish)
- Mobile-native rework of non-Remote Content Manager tabs (those get a *responsive baseline* pass, not a redesign)

## High-level architecture

```
┌────────┐  WSS    ┌──────────┐    Unix   ┌──────────────┐    evdev   ┌─────────────┐
│ Phone  │ ──────▶ │  Flask   │ ────────▶ │ /dev/uinput  │ ─────────▶ │  C++ kiosk  │
│ browser│ ◀────── │ admin.py │ ◀──────── │ virtual dev  │            │ InputManager│
└────────┘  events └──────────┘   status  └──────────────┘            └─────────────┘
                       │  ▲                                                    │
                       │  └────── reads kiosk_status.json @ 5 Hz ──────────────┘
                       │
                       └─ serves admin pages, /pair, /admin/remote/ws
```

Three components, each with a single responsibility:

1. **C++ pairing screen + status writer.** The kiosk owns pairing-code generation (with cryptographic RNG and rotation), QR rendering, the paired-device list display, and writing `kiosk_status.json` at 5 Hz. It also polls a `seek_request.json` queue file each frame to handle phone-initiated tap-to-seek. The kiosk has *no* network code, *no* uinput code, and *no* WebSocket dependency.

2. **Linux uinput virtual device.** Owned by Flask. Registers as a generic gamepad emitting evdev key codes that `InputManager` already maps to `InputAction` values. To the kiosk, the phone is indistinguishable from a USB controller — every existing input behavior (hold-to-repeat, rotary acceleration, sequence detector, controller-aware mapping in `InputManager`) works unchanged.

3. **Flask admin server (existing).** Gains the `/pair` handler, `/admin/remote/ws` WebSocket endpoint, a uinput writer, a status-broadcaster background thread, and the phone-side HTML/CSS/JS bundle served from `static/remote/`. The Remote tab is a sibling of the existing admin tabs.

Why this shape: the kiosk side stays tiny and easy to back out (~300 LOC of pure C++ with no new dependencies). Flask owns all network and protocol logic, where it's easiest to iterate and reuses an existing security surface. Latency through Python is ~5–15 ms on the Pi 4B — well inside the 80 ms p95 budget.

## Pairing & token lifecycle

### Code generation (kiosk)

When the user opens **Settings → Phone Remote** on the kiosk, the C++ pairing screen writes:

```json
// data/pairing_session.json
{
  "schema": 1,
  "code": "847291",            // 6-digit decimal, cryptographic RNG
  "issued_at": 1777772580,
  "expires_at": 1777772700,    // +120 s
  "attempts_remaining": 5,
  "nonce": "<32-byte hex>"
}
```

Atomic write (temp + rename). Auto-rotates every 120 s while the screen is open. Closing the screen deletes the file. Flask treats the file as the single source of truth for what's currently a valid pairing window.

**Edit ownership:** the kiosk owns *generation* (writing fresh codes, rotating, deleting on screen exit). Flask is allowed to mutate exactly one field — `attempts_remaining` — atomically, and to delete the file when that counter reaches zero. The kiosk polls the file at 1 Hz while the pairing screen is open; if it's been deleted (5 wrong attempts) the kiosk regenerates a fresh window and re-renders the QR. Both writers use temp + rename so neither sees a partial file.

### QR contents

```
http://magicpi-XXXX.local:5000/?pair=847291&tab=remote
```

Falls back to `http://10.55.0.1:5000/...` when the USB-C gadget link is the only active interface, mirroring the existing INFO screen's selection logic.

### `/pair` flow (Flask)

1. Read `pairing_session.json`. If missing or `now > expires_at` → 410 Gone. Render: "Pairing screen not open on the kiosk — open Settings → Phone Remote and rescan."
2. Constant-time compare submitted code against stored code.
3. On mismatch: decrement `attempts_remaining`, atomically rewrite the file. After 5 wrong attempts, delete the file; the kiosk polls and re-rolls.
4. On match: generate `device_id = uuid4()`, prompt for a nickname (default = User-Agent device hint, e.g., "iPhone"), append to `data/paired_remotes.json`, issue an HMAC-signed cookie, redirect to `/?tab=remote`. Pairing session is consumed and deleted.

### Cookie format

- Name: `mdb_remote`
- Flags: `HttpOnly`, `SameSite=Strict`, 1-year max-age, path `/`
- Payload: `<device_id>.<issued_at>.<hmac_sig>` where `hmac_sig = HMAC-SHA256(SECRET_KEY, device_id || issued_at)`
- Reuses Flask's existing `SECRET_KEY` (already used for session signing) — no new secret to manage

On every protected request, Flask:

1. Splits and verifies HMAC (constant-time)
2. Checks `device_id` against `paired_remotes.json` — if absent (revoked), reject with 401
3. Updates `last_seen` for that device

### `paired_remotes.json`

```json
{
  "schema": 1,
  "devices": [
    {
      "id": "f3a2…",
      "nickname": "iPhone (Alex)",
      "user_agent_hint": "iPhone",
      "paired_at": 1777772580,
      "last_seen": 1777890000
    }
  ]
}
```

Read by both Flask (auth lookup) and C++ kiosk (display in the Phone Remote screen). "Forget device" on the kiosk just removes the entry; on the phone's next request the cookie HMAC still verifies but the device-id lookup misses → 401 → phone shows "This remote has been unpaired" with a "Pair Again" CTA.

### Brute-force / abuse posture

- 6-digit code + 5-attempt cap + 120 s expiry → effectively unguessable on a LAN
- All `/pair` and `/admin/remote/ws` endpoints rate-limited per source IP (10 req/s, sliding window, in-memory)
- Pairing endpoint logs every attempt (timestamp + outcome + source IP) to `data/pairing_audit.log`

### File ownership summary

| File | Writer | Reader | Purpose |
|------|--------|--------|---------|
| `data/pairing_session.json` | C++ kiosk (while screen open) | Flask `/pair` | Active pairing window |
| `data/paired_remotes.json` | Flask (on pair / nickname edit / revoke) | Flask (auth), C++ kiosk (display) | Device list |
| `data/pairing_audit.log` | Flask | C++ kiosk (display, optional) | Audit trail |
| `data/kiosk_status.json` | C++ kiosk @ 5 Hz | Flask broadcaster | Now Playing state |
| `data/seek_request.json` | Flask (on tap-seek) | C++ kiosk (each tick, drains) | Tap-seek queue |

## Control surface

### Visual style — locked

Style A · Marquee theme · D-pad variant 1 (OK 56 px, arms 70 px wide). This is the **main Remote layout** used in `playlist`, `playback`, `settings`, and `media_browser` screens. The `retroarch` screen is a separate single-purpose layout (described below in *Mode-aware UI*) that reuses the same color tokens but does not share the D-pad shape — it shows only the ROM title and a single Quit button.

Exact tokens for the main layout:

- Phone background: `#1F191F` (kiosk `theme.bg`)
- Off-bg fill: `#2A232A` (kiosk `theme.bg_lift`) — used for D-pad arms, scrub trough, Now Playing strip
- Text: `#F2E4D9` (kiosk `theme.fg`)
- OK center: `#F5BF42` (kiosk `theme.highlight3` / accent gold), 56 px circle, 4 px outer ring of bg color
- Yellow PREV: `#F5BF42` solid, 10 px radius, dark text (`#1F191F`)
- Red PAUSE: `#EA3A27` solid, 10 px radius, cream text (`#F2E4D9`)
- Green NEXT: `#66DD7A` solid, 10 px radius, dark text
- Black MENU: `#2A232A` with `#968B85` 1 px border, cream text
- Layout (top to bottom): Now Playing strip → 4 px scrub bar → 4 px scrub-time row → centered D-pad in flex region → 4×1 colored button grid at bottom

Mockup reference: `.superpowers/brainstorm/27559-1777772548/content/remote-layout-v2.html` (variant 1).

### WS event protocol

Server → phone:

```json
{ "t": "status", "data": { ...full kiosk_status.json snapshot... } }
{ "t": "ack",    "of": "seek", "ok": true }
{ "t": "error",  "code": "rate_limited", "msg": "..." }
```

Phone → server:

```json
{ "t": "press", "btn": "OK", "phase": "down" }    // also "up" for hold-aware
{ "t": "press", "btn": "OK", "phase": "tap" }     // shorthand: down+up in one msg
{ "t": "seek",  "pos": 0.428 }                    // 0.0–1.0, fraction of duration
{ "t": "hello", "client": "remote-v1", "schema": 1 }
```

`btn` is one of: `OK`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `YELLOW`, `RED`, `GREEN`, `BLACK`, `QUIT_GAME`.

`phase: "down"` / `phase: "up"` exists so the kiosk's existing hold-to-repeat (`DPAD_REPEAT_SLOW_HZ = 8 Hz`, accelerating) works correctly — Flask emits a uinput keydown on `down`, keyup on `up`, kiosk does the rest. `tap` is convenience for non-repeating presses.

### Button-to-InputAction mapping

| Phone control | uinput emits | InputAction | Physical box equivalent |
|---|---|---|---|
| OK (gold center) | `BTN_SOUTH` | `SELECT` | rotary push |
| D-pad ↑ / ↓ | `ABS_HAT0Y` ∓1 | `ROTATE_VERTICAL` | rotary turn |
| D-pad ← / → | `ABS_HAT0X` ∓1 | `ROTATE` | axis tilt |
| Yellow (PREV) | `BTN_TL` | `PREV` | BTN1 yellow |
| Red (PAUSE) | `BTN_EAST` | `PLAY_PAUSE` | BTN2 red |
| Green (NEXT) | `BTN_TR` | `NEXT` | BTN3 green |
| Black (MENU) | `BTN_NORTH` | `SETTINGS_MENU` | BTN4 black |

Exact codes will be verified against `input_manager.cpp`'s mapping table during implementation. Principle: pick codes already mapped by `InputManager` so the semantic mapping is one-to-one.

### Tap-to-seek

Tap-to-seek can't ride uinput cleanly (uinput is event-stream, not absolute). Two-channel design:

- **`press`** → uinput key (existing input pipeline, no C++ change)
- **`seek`** → Flask writes `data/seek_request.json` (atomic write, last-write-wins). The kiosk's `Controller::tick()` polls this file every iteration (~16 ms at 60 fps), executes the seek, deletes the file. ~30 LOC added to `controller.cpp`. **Concurrency:** if the user taps the scrub bar twice before the kiosk reads, Flask's second write overwrites the first — the most recent seek wins. This matches user expectation (intermediate seeks shouldn't be honored) and avoids any need for a queue.

### Mode-aware UI

The phone reads `kiosk_status.json.screen` (pushed via WS @ 5 Hz) and adapts:

- **`screen: "playlist"`** — Now Playing shows current playlist + selected item. Scrub bar replaced with thin "X of Y" indicator. All four colored buttons enabled with nav semantics.
- **`screen: "playback"`** — Now Playing shows item title + paused/playing dot. Scrub bar interactive (tap-to-seek). Red button label flips PAUSE/PLAY based on state. D-pad ←/→ reassigned to seek ±5 s in this mode (matches kiosk's existing C-stick behavior; the kiosk's `Controller` already remaps `ROTATE` to seek in playback).
- **`screen: "settings"`** — Now Playing shows "Settings". Scrub bar hidden. Colored buttons act per their existing settings-menu semantics.
- **`screen: "retroarch"`** — Phone takes over with a single-purpose screen: dimmed background, current ROM title, one big red "Quit Game" button. All other controls disabled. Quit injects the existing `Z + Start` hotkey via uinput (`KEY_Z` + `BTN_START`); RetroArch interprets this as "exit core" and the kiosk's wait-on-RetroArch logic returns control naturally.
- **`screen: "media_browser"`** — Same control set as `playlist`. The kiosk's MB screen already handles the same `InputAction` values that the colored buttons emit; no phone-side special-casing needed.

### Connection state on the phone

A small dot in the top-left of the Now Playing strip:

- **Green** — WS connected, last status update <1 s ago
- **Amber** — WS connected but no status update for 1–5 s
- **Red + banner "Reconnecting…"** — WS disconnected. Auto-retries with exponential backoff (250 ms, 500 ms, 1 s, 2 s, max 5 s).
- **Locked screen** — cookie rejected (revoked or invalid). "This remote has been unpaired" with "Pair Again" CTA.

## Status sync & WS push

### `kiosk_status.json` schema

```json
{
  "schema": 1,
  "ts": 1777890000.234,
  "screen": "playback",
  "playlist": { "name": "Movies", "item_index": 3, "item_count": 12 },
  "now_playing": {
    "title": "Tron: Legacy (2010)",
    "subtitle": "Movies",
    "kind": "video"
  },
  "playback": {
    "position_sec": 2538.4,
    "duration_sec": 6900.0,
    "is_paused": false
  },
  "retroarch": null
}
```

`schema` bumps on any breaking field change. Inapplicable sub-objects are `null` (not omitted) so the phone state machine doesn't have to differentiate "missing" from "absent". Atomic write (temp + rename).

### Flask broadcast thread

A single background thread per Flask process watches `kiosk_status.json` via `mtime` check at 5 Hz. mtime-only check skips the JSON parse on idle frames. On change, parses and broadcasts to all connected phone WS clients (multi-phone supported — broadcast is fan-out).

## Settings menu placement

Top-level "Phone Remote" entry in the Settings menu, peer of "Content Manager." Shape mirrors the existing Content Manager entry: top-level item drills into a dedicated full-screen settings sub-screen.

```
Settings
├─ Content Manager     → existing INFO screen (admin URL + QR)
├─ Phone Remote        → new pairing screen
├─ Display
├─ Audio
├─ System
├─ Wi-Fi
├─ Video Games
└─ Back
```

The pairing screen renders:

- Big QR code (same `Renderer::render_qr_code()` helper Content Manager uses, same size)
- 6-digit code in plain text below the QR (manual fallback)
- "Code expires in: M:SS" countdown
- Paired devices list at the bottom: nickname + last-seen + "Forget" affordance per row

Auto-refresh follows the same 1 Hz tick the existing INFO screen uses (see `settings_menu.h:130` comment) so paired-device list updates as new phones complete pairing.

## Mobile-native shell

### Remote tab (mobile-first)

- **Full-viewport takeover** at `(max-width: 700px) AND (pointer: coarse)`. Admin chrome (header, tab strip) hides; D-pad UI fills the viewport with `100dvh` (dynamic viewport height — handles iOS browser chrome correctly).
- **Safe-area insets respected**: `padding: env(safe-area-inset-top) … inset-left)`. Notch / home indicator handled.
- **Touch handling**: `touch-action: manipulation` on all buttons (no double-tap zoom), `user-select: none` on the D-pad area, `overscroll-behavior: none` on body.
- **Press feedback**: `pointerdown` → button transforms to `scale(0.96)` + brightens, `pointerup` → reverts. CSS transitions only. Optional `navigator.vibrate(8)` haptic where supported.
- **PWA manifest**: `manifest.json` with `"display": "standalone"`, kiosk app icon, `theme_color: "#1F191F"`. User can "Add to Home Screen" → launches without browser chrome.
- **Connection-state dot** — top-left of Now Playing strip, color follows the connection state from above. CSS-only animation.
- **No router needed** — single-page; mode transitions are state-driven re-renders of the same DOM.

### Other Content Manager tabs (responsive baseline only — scope A)

- Tab strip becomes horizontally-scrollable pill bar at `max-width: 700px`. Active tab is the only one rendered.
- All tap targets ≥ 44 × 44 px.
- Existing tables get `overflow-x: auto` wrappers so they can scroll sideways without breaking layout.
- Heavy desktop flows (drag-drop upload, playlist drag-builder) keep their desktop UX. On small viewports they show a "Best on a laptop or tablet" hint with a simplified one-at-a-time fallback for must-haves.

## File-level breakdown

### C++ kiosk (`magic_dingus_box_cpp/src/`)

- 🆕 `ui/pairing_screen.{h,cpp}` — QR + code + device list rendering, ~250 LOC
- ✏️ `ui/settings_menu.{h,cpp}` — add `PHONE_REMOTE` `MenuSection`, top-level item, route to pairing screen
- ✏️ `app/controller.{h,cpp}` — write `kiosk_status.json` @ 5 Hz, poll `seek_request.json` each tick (~30 LOC)
- ✏️ `app/app_state.h` — add `ScreenMode` enum and accessor

### Flask (`magic_dingus_box/web/`)

- 🆕 `remote/uinput_writer.py` — virtual device registration + key emit, ~120 LOC
- 🆕 `remote/auth.py` — `/pair` handler, HMAC cookie issue/verify
- 🆕 `remote/ws_handler.py` — WS protocol & multi-client broadcast
- 🆕 `remote/status_broadcaster.py` — `kiosk_status.json` watcher
- ✏️ `admin.py` — register the new blueprint, mount routes, add Remote tab to admin shell
- 🆕 `static/remote/remote.{html,css,js}` — phone UI
- 🆕 `static/manifest.json` — PWA manifest

### System

- 🆕 udev rule granting `rw` on `/dev/uinput` to the user that runs `magic-dingus-web.service` (the `magic` system user — same user that already runs the Flask admin). Rule lives at `/etc/udev/rules.d/90-magicdingus-uinput.rules` and is re-applied idempotently by `setup_services.sh`.
- ✏️ `scripts/install_deps.sh` — `apt install python3-evdev` (provides uinput too)
- ✏️ `scripts/setup_services.sh` — service file capability config

## Build sequence

Each step ends in something verifiable on a real Pi before moving on:

1. **C++ status writer** — kiosk writes `kiosk_status.json`. No Flask side yet. Verify file is correct.
2. **Flask uinput writer + bare WS** — keyboard-emulated remote (curl to send presses) drives the kiosk through uinput. Proves the input pipeline end-to-end before any UI work.
3. **C++ pairing screen + Flask `/pair` + cookie issue** — full pairing flow, no UI yet. Test by visiting `…?pair=CODE` in a browser and asserting the cookie is set.
4. **Phone UI (HTML/CSS/JS)** — D-pad, OK, colored buttons, scrub bar. Connects to the WS from step 2 with the cookie from step 3.
5. **Status broadcast** — Flask reads `kiosk_status.json`, pushes to phone. Phone renders Now Playing and mode-aware UI.
6. **`seek_request.json` round-trip** — tap-to-seek end-to-end.
7. **Mobile-native shell pass** — PWA manifest, safe-area insets, full-screen takeover, responsive pass on other admin tabs.
8. **Settings menu wiring + paired-device list rendering** — final UX polish on the kiosk side.

## Testing

- **Unit (pytest):** HMAC roundtrip, brute-force lockout, code-rotation expiry, cookie revoke-by-deletion, uinput event encoding (against fake-device fixture).
- **Integration (pytest + tmpdir):** spawn a fake kiosk that writes status files; full pair → WS → press → uinput-write → assert event flow.
- **Manual on real Pi:** latency measurement (phone tap timestamp → kiosk frame timestamp; target p95 < 80 ms over typical home Wi-Fi), multi-phone simultaneous, Wi-Fi drop & reconnect, Pi reboot persistence (paired devices survive), pair → use → revoke → assert phone is kicked off.

## Risks

- **uinput grab during RetroArch handoff.** The kiosk re-acquires evdev devices after RetroArch exits. If it accidentally grabs the uinput device with `EVIOCGRAB`, Flask's writes get queued behind it. **Mitigation:** filter devices in `InputManager::open_joystick_devices` to exclude `phys` strings starting with `flask-remote/`.
- **Wi-Fi latency on contended networks.** 80 ms p95 is target, not guarantee. **Mitigation if missed:** enable WS compression and trim the status payload.
- **PWA install discoverability on iOS.** Apple deliberately makes "Add to Home Screen" hard to find. **Mitigation:** one-time toast on first remote use ("Add to Home Screen for full-screen app feel") with a link to a screenshot guide.
- **uinput permissions on Bookworm OS upgrades.** The udev rule could be overwritten or stop working across major Pi OS upgrades. **Mitigation:** include the rule check in `setup_services.sh`'s idempotent re-apply step.

## References

- Mockup (variant 1, locked): `.superpowers/brainstorm/27559-1777772548/content/remote-layout-v2.html`
- Existing QR primitive: `magic_dingus_box_cpp/src/ui/qrcodegen.{cpp,hpp}` and `Renderer::render_qr_code()` at `src/ui/renderer.cpp:4092`
- Existing Settings INFO screen pattern (mirror reference): `magic_dingus_box_cpp/src/ui/settings_menu.cpp:751` `build_info_submenu()`
- Existing input pipeline: `magic_dingus_box_cpp/src/platform/input_manager.{h,cpp}`
- Existing Flask admin: `magic_dingus_box/web/admin.py`
