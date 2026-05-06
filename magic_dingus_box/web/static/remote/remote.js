(function () {
  'use strict';

  const screen = document.getElementById('app');
  const dot = document.getElementById('conn-dot');
  const npLabel = document.getElementById('np-label');
  const npTitle = document.getElementById('np-title');
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
      textInput.disabled = false;
    };
    ws.onmessage = (e) => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      if (msg.t === 'status') applyStatus(msg.data);
    };
    ws.onclose = () => {
      dot.dataset.state = 'red';
      textInput.disabled = true;
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
  function syncToKiosk(newVal, oldVal) {
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
    // Multi-char change — paste, multi-delete, IME composition commit.
    // Cheapest robust path: clear the kiosk buffer and retype everything.
    send({ t: 'clear' });
    for (const c of newVal) {
      // The WS handler filters non-ASCII anyway, but emit cleanly here too.
      if (c.length === 1 && c.charCodeAt(0) < 0x80) {
        send({ t: 'type_char', c });
      }
    }
  }

  function bindPress(el, btn) {
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      send({ t: 'press', btn: btn, phase: 'down' });
      if (typeof navigator.vibrate === 'function') navigator.vibrate(8);
    });
    const release = () => send({ t: 'press', btn: btn, phase: 'up' });
    el.addEventListener('pointerup', release);
    el.addEventListener('pointercancel', release);
    el.addEventListener('pointerleave', release);
  }

  document.querySelectorAll('[data-btn]').forEach((el) => {
    bindPress(el, el.dataset.btn);
  });

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

    const np = s.now_playing || {};
    if (s.screen === 'playback') {
      npLabel.textContent = 'Now Playing';
    } else if (s.screen === 'settings') {
      npLabel.textContent = 'Settings';
    } else if (s.playlist && s.playlist.name) {
      npLabel.textContent = s.playlist.name;
    } else {
      npLabel.textContent = 'Playlist';
    }
    npTitle.textContent = np.title || '—';

    if (s.playback) {
      const p = s.playback;
      const pct = p.duration_sec ? (100 * p.position_sec / p.duration_sec) : 0;
      scrubFill.style.width = pct + '%';
      timeNow.textContent = fmt(p.position_sec);
      timeTotal.textContent = fmt(p.duration_sec);
      btnRed.textContent = p.is_paused ? 'PLAY' : 'PAUSE';
    }

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

  // Tap-to-seek (works only in playback mode; server-side action lands in Phase F).
  scrub.addEventListener('click', (e) => {
    if (screen.dataset.mode !== 'playback') return;
    const rect = e.currentTarget.getBoundingClientRect();
    const pos = (e.clientX - rect.left) / rect.width;
    send({ t: 'seek', pos: Math.max(0, Math.min(1, pos)) });
  });

  // Every native-keyboard input event (per character, paste, autocomplete-
  // commit) fires this. We compute the diff against lastLocalValue and
  // emit the matching kiosk message(s).
  textInput.addEventListener('input', (e) => {
    const newVal = e.target.value;
    syncToKiosk(newVal, lastLocalValue);
    lastLocalValue = newVal;
  });

  // The OS keyboard's "Search" / "Return" key. Submit semantics depend
  // on the kiosk-side context: search keyboard ignores it (no on_enter
  // callback set; search is debounced); WiFi keyboard fires its
  // on_enter (commit password attempt).
  textInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();      // don't submit a form / add a newline
      send({ t: 'key_special', k: 'enter' });
    }
  });

  // "×" clear affordance.
  clearBtn.addEventListener('click', () => {
    textInput.value = '';
    lastLocalValue = '';
    send({ t: 'clear' });
    textInput.focus();    // keep keyboard up so user can keep typing
  });

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
