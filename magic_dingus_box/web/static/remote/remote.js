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
    };
    ws.onmessage = (e) => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      if (msg.t === 'status') applyStatus(msg.data);
    };
    ws.onclose = () => {
      dot.dataset.state = 'red';
      setTimeout(connect, backoff);
      backoff = Math.min(backoff * 2, 5000);
    };
    ws.onerror = () => {};
  }

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
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
