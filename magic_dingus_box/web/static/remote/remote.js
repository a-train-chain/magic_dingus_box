(function () {
  'use strict';

  const screen = document.getElementById('app');
  const dot = document.getElementById('conn-dot');
  const npLabel = document.getElementById('np-label');
  const npTitle = document.getElementById('np-title');
  const npSource = document.getElementById('np-source');
  const scrubFill = document.getElementById('scrub-fill');
  const timeNow = document.getElementById('time-now');
  const timeTotal = document.getElementById('time-total');
  const btnRed = document.getElementById('btn-red');
  const scrub = document.getElementById('scrub');

  // ── Phone Remote — text input ──────────────────────────────────────
  const textSection = document.getElementById('text-section');
  const textInput   = document.getElementById('text-input');
  const textTitle   = document.getElementById('text-title');
  const clearBtn    = document.getElementById('text-clear');

  // The last value we sent to the kiosk. Used to compute per-keystroke
  // diffs (single-char append → type_char, single-char delete →
  // backspace, anything else → clear+retype). The kiosk's authoritative
  // buffer is read back via status; we only override our local copy
  // when our <input> is unfocused (otherwise we'd clobber the user's
  // cursor mid-typing).
  let lastLocalValue = '';

  let ws = null;
  let backoff = 250;
  let lastStatusTs = 0;

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(proto + '//' + location.host + '/admin/remote/ws');
    ws.onopen = () => {
      backoff = 250;
      ws.send(JSON.stringify({ t: 'hello', client: 'remote-v1', schema: 1 }));
      dot.dataset.state = 'green';
      // Cache-safety: if the iOS PWA is serving an OLD HTML (without the
      // text-input element) alongside this newer JS, textInput is null.
      // Don't throw — just skip the disabled toggle. The user will get a
      // working D-pad; refreshing fixes the cache mismatch on their end.
      if (textInput) textInput.disabled = false;
    };
    ws.onmessage = (e) => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      if (msg.t === 'status') applyStatus(msg.data);
    };
    ws.onclose = () => {
      dot.dataset.state = 'red';
      if (textInput) textInput.disabled = true;
      setTimeout(connect, backoff);
      backoff = Math.min(backoff * 2, 5000);
    };
    ws.onerror = () => {};
  }

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  }

  // Compute the diff between the input's previous and current value;
  // emit the WS message(s) that get the kiosk's buffer to match.
  //   - Single-char append → {t: "type_char", c}
  //   - Single-char delete (from the end) → {t: "key_special", k: "backspace"}
  //   - Anything else (paste, multi-delete, IME) → {t: "clear"} + per-char type_chars
  const isAscii = (s) => {
    for (let i = 0; i < s.length; i++) {
      if (s.charCodeAt(i) >= 0x80) return false;
    }
    return true;
  };

  function syncToKiosk(newVal, oldVal) {
    // The kiosk buffer can only hold ASCII (the WS handler + TextInputWriter
    // drop everything else), so the single-char append/delete fast paths are
    // only valid when BOTH strings are fully ASCII. If either side carries a
    // non-ASCII char (a smart quote, em-dash, or accented letter the iOS
    // keyboard/autocorrect inserts), the phone field and the kiosk buffer no
    // longer line up 1:1 — a fast-path backspace would then delete a REAL
    // character the user never touched. In that case fall through to a full
    // clear + ASCII-only retype, which deterministically resyncs the kiosk to
    // the ASCII projection of the field.
    if (isAscii(newVal) && isAscii(oldVal)) {
      // Single-char append at end?
      if (newVal.length === oldVal.length + 1 && newVal.startsWith(oldVal)) {
        send({ t: 'type_char', c: newVal[newVal.length - 1] });
        return;
      }
      // Single-char delete at end?
      if (newVal.length === oldVal.length - 1 && oldVal.startsWith(newVal)) {
        send({ t: 'key_special', k: 'backspace' });
        return;
      }
    }
    // Multi-char change, or any non-ASCII involved — paste, multi-delete,
    // IME/autocorrect commit. Cheapest robust path: clear the kiosk buffer
    // and retype the ASCII-representable characters.
    send({ t: 'clear' });
    for (const c of newVal) {
      if (c.length === 1 && c.charCodeAt(0) < 0x80) {
        send({ t: 'type_char', c });
      }
    }
  }

  // Buttons currently held down on THIS phone, so we can force-release them
  // if the page is backgrounded or hidden (iOS Safari does NOT reliably fire
  // pointerup/pointercancel when the tab backgrounds, the screen locks, or
  // the app is swiped away — leaving the kiosk seeing the button stuck down).
  const heldButtons = new Set();

  function pressDown(btn) {
    heldButtons.add(btn);
    send({ t: 'press', btn: btn, phase: 'down' });
  }
  function pressUp(btn) {
    if (!heldButtons.delete(btn)) return;  // wasn't held; nothing to release
    send({ t: 'press', btn: btn, phase: 'up' });
  }
  function releaseAllHeld() {
    for (const btn of Array.from(heldButtons)) pressUp(btn);
  }

  function bindPress(el, btn) {
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      pressDown(btn);
      if (typeof navigator.vibrate === 'function') navigator.vibrate(8);
    });
    const release = () => pressUp(btn);
    el.addEventListener('pointerup', release);
    el.addEventListener('pointercancel', release);
    el.addEventListener('pointerleave', release);
  }

  document.querySelectorAll('[data-btn]').forEach((el) => {
    bindPress(el, el.dataset.btn);
  });

  // Release everything the moment the page stops being visible or is
  // navigated/backgrounded away. Belt-and-braces with the server-side
  // release-on-disconnect: whichever fires first prevents a stuck input.
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') releaseAllHeld();
  });
  window.addEventListener('pagehide', releaseAllHeld);
  window.addEventListener('blur', releaseAllHeld);

  function applyStatus(s) {
    lastStatusTs = Date.now();
    screen.dataset.mode = s.screen || 'playlist';

    // RetroArch takeover overlay
    const ra = document.getElementById('ra-overlay');
    if (s.screen === 'retroarch') {
      ra.hidden = false;
      const raTitle = document.getElementById('ra-title');
      raTitle.textContent = (s.retroarch && s.retroarch.rom_name)
        ? s.retroarch.rom_name
        : 'Game in progress';
    } else {
      ra.hidden = true;
    }

    // A film is LOADED (playing OR PAUSED). Do NOT use s.screen for this.
    // main.cpp:3557 derives screen from controller.is_playing(), and
    // GstPlayer::update_state() sets is_playing_ = (state == GST_STATE_PLAYING)
    // — a PAUSED pipeline reports NOT playing, so `screen` collapses back to
    // 'playlist' the instant the user pauses, even though a film is still up.
    // Three separate things read that signal and all three were wrong while
    // paused: the centre-key label, the status line, and tap-to-seek.
    // duration_sec is the honest test: GstPlayer::stop() zeroes it, and
    // entering/leaving the Media Browser both call controller.stop().
    const pb = s.playback || {};
    const mediaLoaded = (pb.duration_sec || 0) > 0;

    const np = s.now_playing || {};
    // The status line now reads as a source/context strip; the title has
    // its own large slot below it.
    if (s.screen === 'playback' || mediaLoaded) {
      npLabel.textContent = 'NOW PLAYING';
    } else if (s.screen === 'settings') {
      npLabel.textContent = 'SETTINGS';
    } else if (s.playlist && s.playlist.name) {
      npLabel.textContent = s.playlist.name.toUpperCase();
    } else {
      npLabel.textContent = 'CONNECTED';
    }
    // Idle vs playing. "NOW PLAYING / —" over 0:00/0:00 reads as a broken
    // app rather than an idle one, so say so plainly and hide the scrub.
    const p = pb;
    const hasMedia = mediaLoaded;
    screen.classList.toggle('idle', !hasMedia);

    npTitle.textContent = hasMedia ? (np.title || '—') : 'Nothing playing';
    if (npSource) npSource.textContent = (np.subtitle || '').toUpperCase();

    if (s.playback) {
      const pct = hasMedia ? (100 * p.position_sec / p.duration_sec) : 0;
      scrubFill.style.width = pct + '%';
      timeNow.textContent = fmt(p.position_sec);
      timeTotal.textContent = fmt(p.duration_sec);
      // btn-red is the GLYPH above the red keycap, not the cap itself —
      // writing textContent onto the button would wipe its LED span.
      // Show what the key will DO: ▶ resume, ❚❚ pause, and the neutral
      // ▶❚ (as silkscreened on the panel) when there is nothing to act on.
      btnRed.textContent = !hasMedia ? '\u25B6\u275A'
                         : p.is_paused ? '\u25B6'
                                       : '\u275A\u275A';
    }

    // Contextual centre key. Whenever a film is loaded and the overlay is
    // hidden, SELECT does not select — main.cpp's SELECT handler (2623-2627)
    // reveals the overlay and returns early:
    //     if (state.video_active && !state.ui_visible_when_playing) {
    //         state.ui_visible_when_playing = true; ...; break; }
    // so the key and its caption must read "Browse". Settings and RetroArch
    // own the input while they are up and SELECT there really does select,
    // so they are excluded. One class drives label + caption + colour.
    const modal = s.screen === 'settings' || s.screen === 'retroarch';
    const overlayHidden = mediaLoaded && !modal && s.overlay_visible === false;
    screen.classList.toggle('playing', !!overlayHidden);

    applyTextInput(s);
  }

  // Toggle the phone between D-pad mode and text-input mode based on
  // the kiosk's text_input.active flag. When the kiosk leaves text
  // mode, we also blur the <input> to dismiss any open OS keyboard.
  // When the kiosk has truth that doesn't match the phone's local
  // value (e.g., physical-controller typing, reconnect catch-up), we
  // override — but only if the user isn't actively focused on the
  // field (otherwise their cursor would jump mid-keystroke).
  function applyTextInput(status) {
    const ti = status && status.text_input;
    if (!ti) return;

    const wantsTextMode = ti.active === true;
    document.body.classList.toggle('text-mode', wantsTextMode);

    // Cache-safety: if text-input elements are missing (cached pre-feature
    // HTML), the body class still toggles (harmless on its own — CSS just
    // has nothing to reveal) but skip every line that would dereference
    // the missing nodes.
    if (!textInput || !textTitle) return;

    if (!wantsTextMode) {
      // Kiosk left the text context — dismiss any open OS keyboard.
      if (document.activeElement === textInput) {
        textInput.blur();
      }
      // Reset local state so a fresh entry next time starts clean.
      lastLocalValue = '';
      return;
    }

    // Apply server-truth to the field, but only when the user isn't
    // actively typing (focus would mean their cursor would jump).
    textTitle.textContent = ti.title || '';
    if (document.activeElement !== textInput) {
      const buf = ti.buffer || '';
      if (textInput.value !== buf) {
        textInput.value = buf;
        lastLocalValue = buf;
      }
    }
  }

  function fmt(sec) {
    sec = Math.max(0, Math.floor(sec || 0));
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    const s = sec % 60;
    const pad = (n) => String(n).padStart(2, '0');
    return h ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
  }

  // Connection-state dot — amber if no status update for 1–5s, red if >5s.
  setInterval(() => {
    if (ws && ws.readyState === 1) {
      const age = Date.now() - lastStatusTs;
      dot.dataset.state = age > 5000 ? 'red' : age > 1000 ? 'amber' : 'green';
    }
  }, 500);

  // Tap-to-seek. Gated on "the scrub bar is actually showing", NOT on
  // data-mode: `screen` collapses to 'playlist' while paused (see the
  // mediaLoaded note above), which silently killed seeking in the one state
  // where a user is most likely to want it — paused, scrubbing to a spot.
  // .idle is set by the same status tick that hides the scrub row, so this
  // is exactly "there is a duration to seek within".
  scrub.addEventListener('click', (e) => {
    if (screen.classList.contains('idle')) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const pos = (e.clientX - rect.left) / rect.width;
    send({ t: 'seek', pos: Math.max(0, Math.min(1, pos)) });
  });

  // Every native-keyboard input event (per character, paste, autocomplete-
  // commit) fires this. We compute the diff against lastLocalValue and
  // emit the matching kiosk message(s).
  // Cache-safety: skip listener wiring if the HTML being served lacks the
  // text-section markup (e.g., iOS PWA serving cached pre-feature HTML
  // alongside fresh JS). Without these guards, addEventListener on null
  // throws at module-load time, which crashes the whole JS module
  // INCLUDING the WS connect() call — the user would see the phone fail
  // to reconnect with no clear cause.
  if (textInput) {
    textInput.addEventListener('input', (e) => {
      const newVal = e.target.value;
      syncToKiosk(newVal, lastLocalValue);
      lastLocalValue = newVal;
    });

    // The OS keyboard's "Search" / "Return" key. Just dismisses the OS
    // keyboard — does NOT send a "commit" message to the kiosk.
    //
    // Why: previously this fired key_special:enter, which called
    // keyboard.commit() on the kiosk, which closed the kiosk's keyboard
    // (active_ = false). That flipped status.text_input.active to false,
    // which made the phone hide its text section entirely — the user
    // lost both the text field AND D-pad access mid-flow. They had to
    // re-enter Search to get either back.
    //
    // The right semantic: Search is debounced on the kiosk side; the
    // user typing IS the commit. Pressing the OS keyboard's Search key
    // just means "I'm done with the OS keyboard for now" — dismiss it
    // so the D-pad becomes usable below. Text field stays visible (the
    // user can tap to reopen the OS keyboard); kiosk's on-screen
    // keyboard stays open (D-pad can drive it for additional typing or
    // to navigate results).
    //
    // Wi-Fi password commit semantic: handled separately. The user
    // navigates the kiosk's on-screen Submit key via D-pad and presses
    // SELECT — same as the pre-Phone-Remote flow.
    textInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();      // don't submit a form / add a newline
        textInput.blur();        // dismiss OS keyboard, keep field + kiosk keyboard alive
      }
    });
  }

  // "×" clear affordance.
  if (clearBtn && textInput) {
    clearBtn.addEventListener('click', () => {
      textInput.value = '';
      lastLocalValue = '';
      send({ t: 'clear' });
      textInput.focus();    // keep keyboard up so user can keep typing
    });
  }

  function maybeShowInstallHint() {
    const isStandalone = (typeof window.navigator.standalone !== 'undefined' && window.navigator.standalone === true)
                      || window.matchMedia('(display-mode: standalone)').matches;
    if (isStandalone) return;
    const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
    if (!isIOS) return;
    if (localStorage.getItem('mdb_remote_install_hint_shown') === '1') return;
    const t = document.createElement('div');
    t.className = 'install-toast';
    t.innerHTML = '<strong>Add to Home Screen</strong> for full-screen remote.<br>' +
                  'Tap the Share button → Add to Home Screen.';
    t.addEventListener('click', () => {
      t.remove();
      localStorage.setItem('mdb_remote_install_hint_shown', '1');
    });
    document.body.appendChild(t);
  }
  maybeShowInstallHint();

  connect();
})();

// ---------------------------------------------------------------------------
// iOS standalone viewport re-evaluation.
//
// Measured on an installed PWA (iPhone, iOS 18.7): screen.height 932,
// window.screenY 0, innerHeight 873. The web view is top-aligned and 59px
// SHORT of the display — exactly safe-area-inset-top — leaving dead space
// along the bottom that no stylesheet can reach, because a fixed element
// cannot paint outside the layout viewport. That is why resizing .screen
// (100dvh -> 100% -> position:fixed) never helped: .screen was filling its
// viewport perfectly the whole time; the VIEWPORT was wrong.
//
// The metas are correct (viewport-fit=cover + black-translucent, verified
// served). iOS simply computes the standalone window height once at launch,
// gets it wrong, and only recomputes on a resize. Hence the long-standing
// workaround of rotating to landscape and back: the rotation supplies the
// resize, and the window snaps to the full 932.
//
// So supply that resize ourselves. Detaching and re-appending the viewport
// meta makes WebKit re-parse the viewport configuration, which is the
// programmatic equivalent of the rotation without moving the phone.
// Guarded on being short by a real margin so it is a no-op on devices and
// browsers that were correct to begin with, and bounded so it cannot loop.
// ---------------------------------------------------------------------------
function fixStandaloneViewport(onSettled) {
  var standalone = !!window.navigator.standalone ||
    (window.matchMedia && window.matchMedia('(display-mode: standalone)').matches);
  var before = window.innerHeight;
  if (!standalone || !window.screen || !window.screen.height) {
    if (onSettled) onSettled(before, window.innerHeight, 0);
    return;
  }
  var tries = 0;
  (function kick() {
    // 10px of slack: some devices legitimately differ by a pixel or two, and
    // we must not thrash on those.
    if (window.innerHeight >= window.screen.height - 10 || tries >= 5) {
      if (onSettled) onSettled(before, window.innerHeight, tries);
      return;
    }
    tries++;
    var m = document.querySelector('meta[name="viewport"]');
    if (!m) { if (onSettled) onSettled(before, window.innerHeight, tries); return; }
    var content = m.getAttribute('content');
    var parent = m.parentNode;
    parent.removeChild(m);
    // Force a layout flush so WebKit actually drops the stale viewport config
    // rather than coalescing the remove+append into no change at all.
    void document.documentElement.offsetHeight;
    var n = document.createElement('meta');
    n.setAttribute('name', 'viewport');
    n.setAttribute('content', content);
    parent.appendChild(n);
    setTimeout(kick, 120);
  })();
}

// Re-run when the app is restored from the background: iOS re-creates the
// window on resume and can reintroduce the same stale height.
window.addEventListener('pageshow', function () { fixStandaloneViewport(null); });

// ---------------------------------------------------------------------------
// Gesture guard. The shortfall-compensation experiment that used to live here
// is gone: extending .screen below the layout viewport clipped the gold rule
// and the D-pad instead of filling the strip. Only the root BACKGROUND
// propagates into that region (which the texture rule already exploits);
// content and borders are clipped to the viewport regardless.
// ---------------------------------------------------------------------------


// iOS-specific pinch events; belt-and-braces alongside user-scalable=no and
// touch-action, because a remote must never zoom.
['gesturestart', 'gesturechange', 'gestureend'].forEach(function (t) {
  document.addEventListener(t, function (e) { e.preventDefault(); }, { passive: false });
});
