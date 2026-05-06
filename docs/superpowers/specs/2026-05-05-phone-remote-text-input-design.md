# Phone Remote — Text Input Design

**Date:** 2026-05-05
**Status:** Design (awaiting plan + implementation)
**Companion:** Extends `2026-05-02-phone-remote-design.md`

## Goal

Let the operator type into kiosk text fields (Media Browser search, Wi-Fi
password) using their phone's native OS keyboard, instead of navigating
the kiosk's on-screen keyboard with a D-pad.

## Why

The on-screen keyboard works but is slow — every character is a
sequence of D-pad presses. Operators connecting from a phone already
have a fast, accurate keyboard a tap away (iOS / Android native). This
turns the existing Phone Remote into a more capable input device for
the two text-entry surfaces the kiosk has today.

## Scope

In:
- **MB Search screen** text input
- **Kiosk Wi-Fi password** virtual keyboard (used in Settings)
- Auto-detection so the phone exposes its text input only while the
  kiosk has a text context active
- Live-streaming each keystroke (search results / password buffer
  update in real time)

Out (deliberately):
- Hiding the on-screen kiosk keyboard when a phone is connected — both
  inputs coexist; the phone is purely additive.
- Mirroring kiosk search results on the phone — phone is an input
  device only.
- Multi-line text fields. Both surfaces are single-line.
- Multi-byte / non-ASCII characters. Both buffers are ASCII on the
  kiosk side; non-ASCII is filtered at the Flask edge.

## Architecture

The kiosk already owns one class — `::ui::VirtualKeyboard` — used by
both surfaces. The MB Search screen owns its own instance for the
search field; the kiosk's main settings/Wi-Fi flow owns another.
Whichever instance is currently `is_active()` at any moment is the
"text input receiver" — the destination for phone-typed characters.

A new pointer in `AppState`, `active_text_keyboard`, tracks that.
`StatusWriter` exposes its presence and current buffer to phones via a
new `text_input` block in the 5 Hz status broadcast. Phones swap their
UI from D-pad to text-mode based on that signal.

Phones send keystrokes over the existing WebSocket as new JSON message
types (`type_char`, `key_special`, `clear`). Flask appends them to a
JSON-Lines queue file (`data/text_input_queue.jsonl`). The kiosk's main
loop drains the queue each frame — same pattern as the existing
`seek_request.json` — and dispatches each event to the active keyboard.

Three small additions, no new processes, no new state machines.

## Components

### Kiosk

#### `AppState::active_text_keyboard`

```cpp
// In src/app/app_state.h
ui::VirtualKeyboard* active_text_keyboard = nullptr;
std::string         active_text_title;     // "Search movies", etc.
```

Updated once per frame at the top of `main.cpp`'s render loop:

```cpp
state.active_text_keyboard = nullptr;
state.active_text_title    = "";
#ifdef MEDIA_BROWSER_ENABLED
if (state.current_screen == app::AppScreen::MediaBrowser
    && current_mb_screen == media_browser::ui::Screen::Search
    && mb_search.is_keyboard_active()) {
    state.active_text_keyboard = &mb_search.keyboard();
    state.active_text_title    = "Search movies";
} else
#endif
if (kiosk_keyboard.is_active()) {
    state.active_text_keyboard = &kiosk_keyboard;
    state.active_text_title    = kiosk_keyboard.get_title();
}
```

`mb_search.is_keyboard_active()` and `mb_search.keyboard()` are new
public accessors on the search screen (return its private
`VirtualKeyboard keyboard_`).

#### `VirtualKeyboard` — three new methods

```cpp
// Append a literal character — bypasses the on-screen layout's
// focus-driven select(). Triggers the same on_changed_ callback so
// search-screen debouncer / WiFi UI sees the buffer change.
void type_char(char c);

// Wipe text_buffer_ in one shot, fire on_changed_ once. Used for
// paste handling and the phone's "×" clear affordance.
void clear_buffer();

// Fire on_enter_(text_buffer_). No-op if no on_enter is set
// (MB Search uses live debounce and ignores submit).
void commit();
```

Existing `backspace()`, `space()`, `close()` already do what we need.

#### `text_input_queue.jsonl` drain

New method on `Controller`:

```cpp
// In src/app/controller.h
void poll_text_input_queue(AppState& state);
```

Implementation:

1. `fs::file_size` check on `data/text_input_queue.jsonl`. If zero or
   missing, return immediately (idle fast path).
2. `flock(LOCK_EX)` on the file.
3. Read all lines, parse each as JSON.
4. For each event, if `state.active_text_keyboard != nullptr`,
   dispatch to the keyboard's appropriate method:
   - `type_char` → `keyboard->type_char(c)`
   - `key_special: "backspace"` → `keyboard->backspace()`
   - `key_special: "enter"` → `keyboard->commit()`
   - `key_special: "cancel"` → `keyboard->close()`
   - `clear` → `keyboard->clear_buffer()`

   (Space is sent by the JS as a literal `type_char " "`, so no
   dedicated `key_special: "space"` case is needed. Phone never emits
   it; queue drainer doesn't recognize it.)
5. Truncate file under the same lock.
6. Release lock.

Called once per frame in the main loop, immediately after the existing
`controller.poll_seek_request()`.

#### `StatusWriter` additions

In the JSON snapshot:

```cpp
if (state.active_text_keyboard) {
    out["text_input"] = {
        {"active", true},
        {"title",  state.active_text_title},
        {"buffer", state.active_text_keyboard->get_text()}
    };
} else {
    out["text_input"] = {{"active", false}};
}
```

### Flask web admin

#### `ws_handler.py` — three new message types

The existing `if t == "press"` / `elif t == "seek"` chain extends with:

```python
elif t == "type_char":
    c = msg.get("c", "")
    if len(c) == 1 and c.isprintable() and ord(c) < 0x80:
        text_input_writer.type_char(c, conn.device_id)

elif t == "key_special":
    k = msg.get("k", "")
    if k in ("backspace", "enter", "cancel"):
        text_input_writer.key_special(k, conn.device_id)

elif t == "clear":
    text_input_writer.clear(conn.device_id)
```

#### `text_input_writer.py` (new file)

Single class `TextInputWriter`. Three methods (`type_char`,
`key_special`, `clear`) all append a single line to
`data/text_input_queue.jsonl` under `flock(LOCK_EX)`:

```python
def _append(self, event: dict, device_id: str) -> None:
    event["seq"]    = self._next_seq()      # monotonic per process
    event["ts"]     = time.time()
    event["device"] = device_id              # for logging only
    line = json.dumps(event) + "\n"
    with open(self._path, "ab") as f:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        f.write(line.encode("utf-8"))
        # lock released on close
```

Rate-limit: 50 events/sec/connection (token bucket). Excess silently
dropped to prevent disk-fill.

### Phone (`remote.html` / `remote.css` / `remote.js`)

#### HTML — one new section

```html
<section id="text-section" hidden>
  <header>
    <span id="text-title"></span>
    <button id="text-clear" aria-label="Clear">×</button>
  </header>
  <input id="text-input" type="text" autocomplete="off"
         autocapitalize="none" spellcheck="false" enterkeyhint="search">
</section>
```

`autocomplete=off` and `autocapitalize=none` so iOS doesn't shove
suggestions or capitalize first letter into the kiosk buffer.
`enterkeyhint=search` makes the OS keyboard's return key labeled
"Search" instead of "Return" — small UX touch.

#### CSS — fade-in/out

```css
#text-section { display: none; }
body.text-mode #text-section { display: block; }
#text-section { transition: opacity 150ms ease-out; }
```

D-pad section stays mounted; only the text section appears/disappears.

#### JS — three event listeners + status reconciliation

```js
let lastLocalValue = "";

inputEl.addEventListener("input", (e) => {
  const newVal = e.target.value;
  syncToKiosk(newVal, lastLocalValue);
  lastLocalValue = newVal;
});

inputEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter") {
    e.preventDefault();
    send({ t: "key_special", k: "enter" });
  }
});

clearBtn.addEventListener("click", () => {
  inputEl.value = "";
  send({ t: "clear" });
  lastLocalValue = "";
});

function syncToKiosk(newVal, oldVal) {
  // Single-char append?
  if (newVal.length === oldVal.length + 1 && newVal.startsWith(oldVal)) {
    send({ t: "type_char", c: newVal[newVal.length - 1] });
    return;
  }
  // Single-char delete?
  if (newVal.length === oldVal.length - 1 && oldVal.startsWith(newVal)) {
    send({ t: "key_special", k: "backspace" });
    return;
  }
  // Multi-char change (paste, multi-delete, IME): clear + retype.
  send({ t: "clear" });
  for (const c of newVal) send({ t: "type_char", c });
}

function applyStatus(status) {
  const ti = status?.text_input;
  if (!ti) return;
  document.body.classList.toggle("text-mode", ti.active === true);
  if (ti.active === false && document.activeElement === inputEl) {
    inputEl.blur();   // dismiss OS keyboard cleanly when kiosk leaves text mode
  }
  if (ti.active && document.activeElement !== inputEl) {
    // Field unfocused — safe to overwrite from server truth.
    inputEl.value = ti.buffer ?? "";
    lastLocalValue = inputEl.value;
  }
  titleEl.textContent = ti.title ?? "";
}
```

`syncToKiosk`'s diff handles paste / multi-delete cleanly — even a
30-character pasted password is at most 31 WS messages, well under any
sane rate limit.

`applyStatus` runs on every status broadcast. The "field unfocused →
trust server" rule keeps phone display in sync with kiosk truth (incl.
mixed input from a physical controller) without clobbering the user's
cursor mid-typing.

## Data flow

```
User types "s" on phone keyboard
  ↓
<input> input event fires
  ↓
JS computes diff → send({t: "type_char", c: "s"})
  ↓
WebSocket → Flask ws_handler.py
  ↓
TextInputWriter.type_char("s", device_id)
  ↓
appended to data/text_input_queue.jsonl
  ↓
Kiosk main loop: controller.poll_text_input_queue(state)
  ↓
parse line → state.active_text_keyboard->type_char('s')
  ↓
keyboard's text_buffer_ updates → on_changed_ fires
  ↓
search debouncer schedules query / WiFi UI re-renders
  ↓
StatusWriter (5Hz) emits new buffer in next status broadcast
  ↓
WebSocket → all connected phones
  ↓
applyStatus updates phone <input>.value (if field unfocused)
```

End-to-end latency on local Wi-Fi: ~30–80 ms per character. Well under
the human-perceivable threshold for typing feedback.

## Per-surface routing

| Phone WS msg | Kiosk method | MB Search behavior | Wi-Fi keyboard behavior |
|---|---|---|---|
| `type_char "x"` | `type_char(c)` | appends to `query_`, debouncer fires search | appends to password buffer |
| `type_char " "` (literal space) | `type_char(' ')` | adds space | adds space |
| `key_special backspace` | `backspace()` | removes last char | removes last char |
| `key_special enter` | `commit()` | no-op (no on_enter set on search keyboard) | fires `on_enter_` → password attempt |
| `key_special cancel` | `close()` | keyboard closes, fires on_cancel | keyboard closes, fires on_cancel |
| `clear` | `clear_buffer()` | wipes query_ | wipes password buffer |

## Edge cases

- **Two phones connected, both typing**: both feed the same buffer.
  Per-message order at Flask is preserved, but inter-phone ordering is
  best-effort. Status broadcast (5 Hz) re-syncs all phones to kiosk
  truth.
- **Physical controller + phone typing simultaneously**: same buffer;
  status broadcast keeps phones in sync (provided their `<input>`
  isn't focused).
- **Phone sends events while kiosk has no text context**: events
  queue, but `state.active_text_keyboard == nullptr` on drain →
  dropped silently. Queue truncated regardless. No backlog.
- **Kiosk leaves text context mid-typing** (operator closes search via
  physical controller): next status push has `active=false`. Phone
  blurs `<input>` (dismisses OS keyboard) and fades the text section
  out. In-flight chars dropped per above.
- **Non-ASCII / emoji**: filtered at Flask edge. `type_char` only
  forwards `len(c)==1 and c.isprintable() and ord(c)<0x80`.
  Search-buffer encoding stays clean.
- **Paste of multi-char content**: JS diff falls through to
  clear-then-retype. Single user action → handful of WS messages.
- **WS disconnects while typing**: phone reconnect-banner shows;
  `<input>` becomes `disabled`. On reconnect, phone re-syncs to kiosk
  buffer (chars typed during outage were buffered locally and are NOT
  replayed — kiosk may have moved on). User re-types if needed.
- **`text_input_queue.jsonl` corruption**: each line parsed
  independently. Bad lines logged + skipped. File truncated post-parse
  regardless.

## Security

- **Cookie auth**: every WS connection still verifies the existing
  `mdb_remote` HMAC cookie. An unpaired phone cannot send `type_char`.
  Same auth surface as `press` / `seek`.
- **No remote code paths**: typed characters feed a fixed
  `text_buffer_` string. The buffer is consumed by either Radarr's
  search HTTP call (URL-escaped via cURL) or `nmcli wifi connect ...
  password` (passed as argv, no shell interpolation). No eval, no
  shell, no path-injection vector.
- **Rate limit**: Flask token-buckets each connection at 50 events/sec.
  Far above sustained human typing (~5–10 char/sec). Prevents a
  malicious paired phone from filling disk via the queue file.

## Testing

### C++ unit (Catch2) — `tests/phone_remote/test_text_input_queue.cpp`

- `Controller::poll_text_input_queue` consumes events in `seq` order.
- Idle fast path: empty file → no parse, no allocation.
- Malformed JSON line → skipped, file still truncated.
- Drop-when-no-receiver: events arriving with
  `active_text_keyboard == nullptr` produce no side effects on any
  keyboard.
- `VirtualKeyboard::type_char(c)` appends to buffer and fires
  `on_changed_` callback.
- `VirtualKeyboard::commit()` fires `on_enter_(buffer)` when set;
  no-ops cleanly when not set.

### Python unit (pytest) — `tests/phone_remote/test_text_input_protocol.py`

- WS handler routes `type_char`, `key_special`, `clear` to the queue
  file with monotonic seq, locking, JSON-per-line format.
- Non-ASCII filter drops emoji / non-printable / 2-byte chars.
- Rate-limit kicks in at 51st event in 1-second window.
- Concurrent writers from two threads produce well-formed file (no
  torn writes — flock works).

### Integration (manual on Pi)

1. **Search**: kiosk on MB Search screen → tap search field on phone
   → OS keyboard pops → type "shawshank" → results appear on kiosk
   live as each character arrives → tap a result on D-pad below.
2. **Wi-Fi**: kiosk on Settings → Wi-Fi → select network → password
   keyboard appears → tap field on phone → type password → tap
   "Search" / Enter on OS keyboard → kiosk attempts connect.
3. **Mode swap**: physically close kiosk's keyboard via controller →
   phone auto-blurs and swaps back to D-pad within 200ms.
4. **Two phones**: type from both simultaneously → final buffer is
   well-formed; both phones display same buffer.

## Migration / rollout

Backward-compatible. Old phones (cached `remote.js`) ignore the new
`text_input` status block and stay in D-pad mode permanently — no
breakage. Force-reload (or wait for cache expiry) lifts them into the
new behavior. No phone re-pairing needed.

The kiosk's existing on-screen keyboard remains usable in parallel,
so operators using a physical controller see no change.

## File touch list

- `magic_dingus_box_cpp/src/ui/virtual_keyboard.{h,cpp}` — add
  `type_char`, `commit` methods.
- `magic_dingus_box_cpp/src/app/app_state.h` — add
  `active_text_keyboard` pointer, `active_text_title` string.
- `magic_dingus_box_cpp/src/app/controller.{h,cpp}` — add
  `poll_text_input_queue(state)`.
- `magic_dingus_box_cpp/src/app/status_writer.cpp` — emit
  `text_input` block.
- `magic_dingus_box_cpp/src/main.cpp` — populate
  `state.active_text_keyboard` per frame; call
  `controller.poll_text_input_queue(state)` per frame.
- `magic_dingus_box_cpp/src/media_browser/ui/search_screen.{h,cpp}`
  — public accessors `is_keyboard_active()`, `keyboard()`.
- `magic_dingus_box/web/remote/text_input_writer.py` — new file.
- `magic_dingus_box/web/remote/ws_handler.py` — three new message
  cases dispatching to `text_input_writer`.
- `magic_dingus_box/web/static/remote/remote.html` — text-section
  markup.
- `magic_dingus_box/web/static/remote/remote.css` — text-mode CSS.
- `magic_dingus_box/web/static/remote/remote.js` — input listeners,
  diff, status reconciliation.
- `magic_dingus_box_cpp/tests/phone_remote/test_text_input_queue.cpp`
  — new file.
- `magic_dingus_box/web/tests/phone_remote/test_text_input_protocol.py`
  — new file.
- `magic_dingus_box_cpp/scripts/deploy_cpp.sh` — add
  `text_input_queue.jsonl` to the rsync-runtime-state exclude list
  (preserves it across deploys; the kiosk truncates it itself).
