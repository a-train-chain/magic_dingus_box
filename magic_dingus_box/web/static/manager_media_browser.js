// Media Browser tab: visibility gate, setup (WireGuard config upload +
// progress), Prepare Drive, TMDB key, status/health, credentials, restart
// and reset. The VPN-country control lives in vpn_settings.js.
//
// Part of the Content Manager front end; see the load-order note at the top
// of manager.js. Loaded after: manager.js, manager_devices.js, manager_media.js, manager_roms.js, manager_playlists.js, manager_import.js, manager_settings.js.

// ===== MEDIA BROWSER (RADARR/PROWLARR/QBIT/GLUETUN) =====
//
// Three UI states (driven by GET /admin/media-browser/status):
//   A. Not configured   → drop-zone for WireGuard .conf
//   B. Configuring      → live tail of setup_services.sh stdout
//   C. Configured       → VPN exit IP + container table + Reconfigure
//
// Mirrors the OTA-update job pattern: POST returns a job_id, frontend polls
// /admin/media-browser/setup-status/<job_id> every 2 sec.

let mbSetupPollHandle = null;   // setInterval id for setup-status polling
let mbForceSetupView = false;   // true when user clicked Reconfigure
let mbConfigPayload = null;     // {file: File} or {text: string}
let mbActiveJobId = null;
let mbSetupStartTs = null;

function _mbShowState(stateName) {
    const ids = ['mbLoadingState', 'mbStateNotConfigured', 'mbStateConfiguring', 'mbStateConfigured'];
    const targetId = {
        loading: 'mbLoadingState',
        notconfigured: 'mbStateNotConfigured',
        configuring: 'mbStateConfiguring',
        configured: 'mbStateConfigured',
    }[stateName];
    ids.forEach(id => {
        const el = document.getElementById(id);
        if (el) el.style.display = (id === targetId) ? 'block' : 'none';
    });

    // The TMDB panel is not one of the three states — it is shown alongside
    // all of them, so the key can be set during setup AND changed afterwards.
    // Hidden only in 'loading', where we don't yet know the tab is usable.
    // Movie Storage is shown alongside every state for the same reason, and
    // one more: with no drive mounted, downloads land on the SD card. That is
    // most likely to be true DURING setup, so the panel has to be reachable
    // before the stack is healthy — not only after.
    const storage = document.getElementById('mbStorageSection');
    if (storage) {
        const showStorage = (stateName !== 'loading');
        storage.style.display = showStorage ? 'block' : 'none';
        if (showStorage) {
            _wireStoragePanel();
            refreshStorageDevices();
        }
    }

    const tmdb = document.getElementById('mbTmdbSection');
    if (tmdb) {
        const show = (stateName !== 'loading');
        tmdb.style.display = show ? 'block' : 'none';
        if (show) {
            // Wire here rather than at DOMContentLoaded: the Media Browser tab
            // is hidden until the kiosk unlock check passes, and the existing
            // panels already re-wire defensively for the same reason.
            _wireTmdbKeyPanel();
            refreshTmdbKeyStatus();
        }
    }
}

// ===== MOVIE STORAGE (PREPARE DRIVE) =====
//
// A customer's new drive arrives exFAT or NTFS with an arbitrary label, so it
// does not match the `LABEL=MOVIES ... ext4` line in /etc/fstab and never
// mounts. ext4 cannot be produced from Windows or macOS, so preparation has to
// happen on the box.
//
// Every safety decision lives server-side in storage_prepare.py — this UI only
// renders what the box says is eligible and collects a typed confirmation. It
// never decides what is safe to erase.

let _mbPrepareTarget = null;
// Last listing from the box, keyed by device name. Kept so the confirm modal
// can read the device's real properties instead of scraping them back out of
// rendered HTML — which would break the moment the markup changed, and would
// break in the direction of showing the WRONG warning.
let _mbStorageDevices = {};

async function refreshStorageDevices() {
    const status = document.getElementById('mbStorageStatus');
    const list = document.getElementById('mbStorageDevices');
    if (!status || !list || !currentDevice) return;

    status.innerHTML = '<span class="loading">Checking storage…</span>';
    list.innerHTML = '';
    try {
        const data = await apiGet(`${currentDevice.url}/admin/media-browser/storage/devices`);
        const devices = data.devices || [];
        _mbStorageDevices = {};
        devices.forEach(d => { _mbStorageDevices[d.name] = d; });
        const prepared = devices.filter(d => d.is_current_movies_drive);

        // Lead with the thing that actually matters to the customer: is there
        // somewhere for movies to go, or are they filling the SD card?
        status.innerHTML = prepared.length
            ? `<div style="color: var(--success, #66DD7A);">Movie drive connected — downloads go to ${escapeHtml(data.mountpoint)}</div>`
            : `<div style="color: var(--error);">No movie drive connected. Downloads would fill the SD card.</div>`;

        if (!devices.length) {
            list.innerHTML = '<p class="settings-description">No drives found. Plug in a USB drive and rescan.</p>';
            return;
        }
        list.innerHTML = devices.map(d => `
            <div class="health-info" style="display:flex; align-items:center; justify-content:space-between; gap:0.75rem; margin-bottom:0.5rem;">
                <div>
                    <div style="font-family: monospace;">${escapeHtml(d.path)} — ${escapeHtml(d.size || '')}</div>
                    <div class="settings-description" style="margin:0;">
                        ${d.is_current_movies_drive
                            ? 'Currently holding your movie library'
                            : (d.fstype ? escapeHtml(d.fstype) : 'unformatted')}
                        ${d.removable ? ' · removable' : ''}
                    </div>
                </div>
                <button class="btn-secondary small" style="color: var(--error); border-color: var(--error); white-space: nowrap;"
                        onclick="openPrepareDriveModal('${escapeHtml(d.name)}')">
                    Erase &amp; prepare
                </button>
            </div>`).join('');
    } catch (e) {
        status.innerHTML = `<div style="color: var(--error);">Could not read storage: ${escapeHtml(e.message || String(e))}</div>`;
    }
}

function openPrepareDriveModal(name) {
    _mbPrepareTarget = name;

    const warn = document.getElementById('mbPrepareWarning');
    const isCurrent = !!(_mbStorageDevices[name] || {}).is_current_movies_drive;
    if (warn) {
        warn.innerHTML = isCurrent
            ? `<p style="color: var(--error);"><strong>This is the drive currently holding your movie library.</strong>
                 Erasing it will permanently delete every downloaded movie on it. This cannot be undone.</p>`
            : `<p>Everything on <strong>/dev/${escapeHtml(name)}</strong> will be permanently erased and
                 replaced with an empty movie library. This cannot be undone.</p>`;
    }
    const label = document.getElementById('mbPrepareDeviceName');
    if (label) label.textContent = name;

    const input = document.getElementById('mbPrepareConfirmInput');
    const btn = document.getElementById('mbPrepareConfirmBtn');
    if (input) { input.value = ''; input.placeholder = `Type ${name}`; }
    if (btn) btn.disabled = true;

    const modal = document.getElementById('mbPrepareModal');
    if (modal) modal.classList.add('active');
    if (input) setTimeout(() => input.focus(), 50);
}

function _closePrepareDriveModal() {
    const modal = document.getElementById('mbPrepareModal');
    if (modal) modal.classList.remove('active');
    _mbPrepareTarget = null;
}

async function _confirmPrepareDrive() {
    const device = _mbPrepareTarget;
    const input = document.getElementById('mbPrepareConfirmInput');
    const statusEl = document.getElementById('mbStorageActionStatus');
    if (!device || !input || input.value.trim() !== device) return;

    _closePrepareDriveModal();
    if (statusEl) {
        statusEl.style.display = 'block';
        statusEl.innerHTML = `<span class="loading">Preparing ${escapeHtml(device)} — this can take a minute…</span>`;
    }
    try {
        const data = await apiRequest(
            `${currentDevice.url}/admin/media-browser/storage/prepare`,
            { method: 'POST', headers: getCsrfHeaders(),
              body: JSON.stringify({ device, confirm: device }) });
        if (statusEl) {
            statusEl.innerHTML = `<span style="color: var(--success, #66DD7A);">${escapeHtml(device)} is ready for movies.</span>`;
        }
        refreshStorageDevices();
    } catch (e) {
        if (statusEl) {
            statusEl.innerHTML = `<span style="color: var(--error);">Failed: ${escapeHtml(e.message || String(e))}</span>`;
        }
    }
}

function _wireStoragePanel() {
    const input = document.getElementById('mbPrepareConfirmInput');
    const btn = document.getElementById('mbPrepareConfirmBtn');
    const cancel = document.getElementById('mbPrepareCancelBtn');
    // Wired here rather than at DOMContentLoaded because the Media Browser tab
    // does not exist in the DOM until the unlock check passes — the same
    // reason the TMDB panel re-wires defensively.
    if (input && !input.dataset.wired) {
        input.dataset.wired = '1';
        input.addEventListener('input', () => {
            if (btn) btn.disabled = (input.value.trim() !== _mbPrepareTarget);
        });
    }
    if (btn && !btn.dataset.wired) {
        btn.dataset.wired = '1';
        btn.addEventListener('click', _confirmPrepareDrive);
    }
    if (cancel && !cancel.dataset.wired) {
        cancel.dataset.wired = '1';
        cancel.addEventListener('click', _closePrepareDriveModal);
    }
}

// ===== TMDB API KEY =====
//
// The kiosk's Browse and Search screens are driven entirely by TMDB, and
// first_boot.sh wipes the developer's key from every cloned unit — so a box
// with no key here has a working downloader and a completely blank discovery
// surface. This panel is the only non-SSH way to fix that.
//
// The key value NEVER reaches this page. The backend reports whether one is
// set and how long it is; everything below renders from that.

let _mbTmdbPendingUnverified = null;  // key awaiting an explicit "save anyway"

function _mbTmdbSetError(message, tone = 'error') {
    const el = document.getElementById('mbTmdbError');
    if (!el) return;
    if (!message) {
        el.style.display = 'none';
        el.textContent = '';
        return;
    }
    el.style.display = 'block';
    el.style.color = (tone === 'error') ? 'var(--error)' : 'var(--success)';
    el.textContent = message;
}

/**
 * Fetch and render the TMDB key state. Never receives the key itself.
 */
async function refreshTmdbKeyStatus() {
    const statusEl = document.getElementById('mbTmdbStatus');
    const noticeEl = document.getElementById('mbTmdbNotice');
    const restartBtn = document.getElementById('mbTmdbRestartBtn');
    if (!statusEl) return;

    if (!currentDevice) {
        statusEl.innerHTML = '<span class="loading">Connect to a device first.</span>';
        return;
    }

    try {
        const d = await apiGet(`${currentDevice.url}/admin/media-browser/tmdb`);

        // Masked readout, matching the Service Credentials rows: a run of dots
        // sized to the real key, and never the key.
        const dots = '•'.repeat(Math.min(32, Math.max(8, d.key_length || 8)));
        statusEl.innerHTML = `
            <div class="health-row">
                <span class="health-label">API key</span>
                <span class="health-value" style="font-family: monospace;">${
                    d.configured
                        ? `${dots} <span style="color: var(--success); font-family: inherit;">configured</span>`
                        : '<span style="color: var(--accent);">not set</span>'
                }</span>
            </div>`;

        const notices = [];
        if (!d.configured) {
            // The empty-state fix, web-side half: say WHY Browse is blank
            // instead of letting the TV's "No movies in this category" imply
            // the box is broken.
            notices.push({
                tone: 'accent',
                html: '<strong>Browse and Search on the TV are empty</strong> because no ' +
                      'TMDB key is set. Add one below and they will fill in.',
            });
        }
        if (d.env_override) {
            // main.cpp checks $MDB_TMDB_API_KEY before the file, so a key set
            // in services/.env silently wins over anything saved here.
            notices.push({
                tone: 'error',
                html: '<strong>MDB_TMDB_API_KEY is set in the environment.</strong> The kiosk ' +
                      'reads that before this file, so saving a key here will have no effect ' +
                      'until it is removed from <code>services/.env</code>.',
            });
        }
        if (d.configured && d.kiosk_has_current_key === false) {
            // THE restart problem, stated plainly rather than papered over.
            notices.push({
                tone: 'accent',
                html: '<strong>The kiosk has not picked this key up yet.</strong> It reads the ' +
                      'key once when it starts, so it is still running with whatever it had ' +
                      'before. Restart it to apply — this interrupts anything playing on the TV.',
            });
        }

        if (noticeEl) {
            if (notices.length) {
                noticeEl.style.display = 'block';
                noticeEl.innerHTML = notices.map(n => `
                    <div class="settings-description" style="color: var(--${n.tone}); margin: 0 0 0.5rem 0;">
                        ${spriteIcon('ic-warning')} ${n.html}
                    </div>`).join('');
            } else {
                noticeEl.style.display = 'none';
                noticeEl.innerHTML = '';
            }
        }

        // Offer the restart only when it would actually change something.
        if (restartBtn) {
            restartBtn.style.display =
                (d.configured && d.kiosk_has_current_key === false && d.kiosk_service_active)
                    ? 'inline-flex' : 'none';
        }
    } catch (e) {
        console.error('TMDB key status fetch failed:', e);
        statusEl.innerHTML =
            `<span class="loading" style="color: var(--error);">Failed to read key status: ${escapeHtml(e.message || String(e))}</span>`;
    }
}

/**
 * Validate + save a TMDB key. The backend checks it against TMDB before it
 * writes anything, so a bad key can never clobber a working one.
 */
async function saveTmdbKey(allowUnverified = false) {
    const input = document.getElementById('mbTmdbInput');
    const saveBtn = document.getElementById('mbTmdbSaveBtn');
    if (!input || !currentDevice) return;

    const key = allowUnverified ? _mbTmdbPendingUnverified : input.value.trim();
    if (!key) {
        _mbTmdbSetError('Enter your TMDB API key.');
        return;
    }

    _mbTmdbSetError('');
    if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = 'Checking key…'; }

    try {
        const d = await apiPost(`${currentDevice.url}/admin/media-browser/tmdb`,
                                { api_key: key, allow_unverified: !!allowUnverified });
        // Clear the field on success — no reason to leave a secret sitting in
        // the DOM, and the masked row above now reflects the saved state.
        input.value = '';
        _mbTmdbPendingUnverified = null;
        _mbTmdbSetError(
            d.verified
                ? 'Key verified with TMDB and saved.'
                : 'Key saved, but it could not be verified — Browse may still be empty if it is wrong.',
            d.verified ? 'success' : 'error');
        await refreshTmdbKeyStatus();
    } catch (e) {
        if (e.code === 'tmdb_unreachable') {
            // No internet is not evidence the key is bad — but saving blind
            // reproduces the silent-empty-Browse failure, so make it a choice.
            _mbTmdbPendingUnverified = key;
            const el = document.getElementById('mbTmdbError');
            if (el) {
                el.style.display = 'block';
                el.style.color = 'var(--accent)';
                el.innerHTML =
                    `${escapeHtml(e.message || 'Could not reach TMDB to check this key.')} ` +
                    `The key looks well-formed. ` +
                    `<button type="button" class="btn-secondary small" id="mbTmdbSaveAnywayBtn" ` +
                    `style="margin-left: 0.5rem;">Save without checking</button>`;
                const btn = document.getElementById('mbTmdbSaveAnywayBtn');
                if (btn) btn.addEventListener('click', () => saveTmdbKey(true));
            }
        } else {
            _mbTmdbSetError(e.message || String(e));
        }
    } finally {
        if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = 'Save Key'; }
    }
}

/**
 * Restart the kiosk so it re-reads the key. Explicit and opt-in: this kills
 * whatever is on the TV, which is why nothing calls it automatically.
 */
async function restartKioskForTmdbKey() {
    if (!currentDevice) return;
    if (!confirm('Restart the kiosk now?\n\nThis interrupts anything playing on the TV. ' +
                 'It takes a few seconds, and afterwards the kiosk will be using the new TMDB key.')) {
        return;
    }
    const btn = document.getElementById('mbTmdbRestartBtn');
    if (btn) { btn.disabled = true; btn.textContent = 'Restarting…'; }
    try {
        await apiPost(`${currentDevice.url}/admin/media-browser/restart-kiosk`, {});
        _mbTmdbSetError('Kiosk restarted — it is now using the saved key.', 'success');
    } catch (e) {
        _mbTmdbSetError(`Restart failed: ${e.message || String(e)}`);
    } finally {
        if (btn) {
            btn.disabled = false;
            btn.innerHTML = `${spriteIcon('ic-refresh')} Restart Kiosk`;
        }
        // Give systemd a moment to record the new start time before we ask
        // whether the kiosk is current again.
        setTimeout(refreshTmdbKeyStatus, 3000);
    }
}

function _wireTmdbKeyPanel() {
    const btn = document.getElementById('mbTmdbRevealBtn');
    const input = document.getElementById('mbTmdbInput');
    if (btn && input && !btn.dataset.wired) {
        btn.dataset.wired = '1';
        btn.addEventListener('click', () => {
            const hidden = input.type === 'password';
            input.type = hidden ? 'text' : 'password';
            btn.textContent = hidden ? 'Hide' : 'Show';
        });
    }
    if (input && !input.dataset.wired) {
        input.dataset.wired = '1';
        // Enter submits — this is a one-field form in all but markup.
        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') { e.preventDefault(); saveTmdbKey(); }
        });
    }
}

async function refreshMediaBrowserStatus() {
    if (!currentDevice) {
        _mbShowState('loading');
        const loading = document.getElementById('mbLoadingState');
        if (loading) loading.querySelector('.section-body').innerHTML = '<span class="loading">Connect to a device first.</span>';
        return;
    }

    // If a setup poll is in progress, stay in configuring view
    if (mbSetupPollHandle !== null) {
        return;
    }

    try {
        const data = await apiGet(`${currentDevice.url}/admin/media-browser/status`);
        _renderMediaBrowserStatus(data);
    } catch (e) {
        console.error('Media Browser status fetch failed:', e);
        _mbShowState('loading');
        const loading = document.getElementById('mbLoadingState');
        if (loading) loading.querySelector('.section-body').innerHTML =
            `<span class="loading" style="color: var(--error);">Failed to fetch status: ${escapeHtml(e.message || String(e))}</span>`;
    }
}

function _renderMediaBrowserStatus(data) {
    // Reset config error if drop-zone is being shown
    const errorEl = document.getElementById('mbConfigError');
    if (errorEl) errorEl.style.display = 'none';

    if (!data.configured || mbForceSetupView) {
        _mbShowState('notconfigured');
        _setupMediaBrowserDropZone();
        return;
    }

    // Configured — render State C
    _mbShowState('configured');

    // Make sure the credentials expander + reset modal are wired (DOM may not
    // have existed at DOMContentLoaded time on slow page loads).
    _wireMediaBrowserCredentialsExpander();
    _wireMediaBrowserResetModal();

    // Reset health summary panel on each State C entry — operator must
    // press Refresh to populate it, by design (no auto-poll).
    const summaryEl = document.getElementById('mbHealthSummary');
    if (summaryEl) {
        summaryEl.innerHTML = '<span class="loading">Click Refresh to load…</span>';
    }
    // Collapse + reset credentials so they don't leak across device switches
    const credExpander = document.getElementById('mbCredentialsExpander');
    if (credExpander) credExpander.open = false;
    mbCredentialsLoaded = false;

    const titleEl = document.getElementById('mbConfiguredTitle');
    if (titleEl) {
        if (data.services_running) {
            titleEl.textContent = '✓ Media Browser Healthy';
            titleEl.style.color = 'var(--success)';
        } else {
            titleEl.textContent = '⚠ Configured but services not running';
            titleEl.style.color = 'var(--accent)';
        }
    }

    // VPN info
    const vpnEl = document.getElementById('mbVpnInfo');
    if (vpnEl) {
        if (data.vpn_country) {
            vpnEl.innerHTML = `
                <div class="health-row">
                    <span class="health-label">VPN country</span>
                    <span class="health-value">${escapeHtml(data.vpn_country || '—')}</span>
                </div>`;
        } else if (data.services_running) {
            vpnEl.innerHTML = `
                <div class="health-row">
                    <span class="health-label">VPN exit IP</span>
                    <span class="health-value" style="color: var(--accent);">Tunnel reported no exit IP yet</span>
                </div>`;
        } else {
            vpnEl.innerHTML = `
                <div class="health-row">
                    <span class="health-label">VPN status</span>
                    <span class="health-value" style="color: var(--accent);">Services not running</span>
                </div>`;
        }
    }

    // Container table
    const tableEl = document.getElementById('mbContainerTable');
    if (tableEl) {
        const rows = (data.containers || []).map(c => {
            // Intentional stop by playback_services_pause.sh — the kiosk
            // frees RAM while a game/movie runs. Not an error: label it
            // honestly, keep the raw docker state as secondary detail
            // (Radarr/Prowlarr always land at "Exited (137)" here).
            if (c.paused_for_playback) {
                return `
                <div class="health-row">
                    <span class="health-label">${escapeHtml(c.name)}</span>
                    <span class="health-value" style="color: var(--accent);">Paused while a game/movie is playing
                        <span style="opacity: 0.6;">— ${escapeHtml(c.status)}</span></span>
                </div>`;
            }
            const isUp = (c.status || '').toLowerCase().startsWith('up');
            const color = isUp ? 'var(--success)' : 'var(--error)';
            return `
                <div class="health-row">
                    <span class="health-label">${escapeHtml(c.name)}</span>
                    <span class="health-value" style="color: ${color};">${escapeHtml(c.status)}</span>
                </div>`;
        }).join('');
        tableEl.innerHTML = rows || '<span class="loading">No containers found</span>';
    }
}

function _setupMediaBrowserDropZone() {
    const fileInput = document.getElementById('mbConfigFile');
    const textArea = document.getElementById('mbConfigText');
    const dropZone = document.getElementById('mbDropZone');
    const dropLabel = document.getElementById('mbDropLabel');
    const button = document.getElementById('mbConfigureBtn');
    const errorEl = document.getElementById('mbConfigError');

    if (!fileInput || !button) return;

    // Idempotency guard — bail if we've already wired listeners up
    if (dropZone && dropZone.dataset.wired === '1') return;
    if (dropZone) dropZone.dataset.wired = '1';

    const updateButtonState = () => {
        const haveFile = fileInput.files && fileInput.files.length > 0;
        const haveText = textArea && textArea.value.trim().length > 0;
        button.disabled = !(haveFile || haveText);
        if (haveFile) {
            mbConfigPayload = { file: fileInput.files[0] };
            if (dropLabel) dropLabel.innerHTML = `${spriteIcon('ic-doc')} ${escapeHtml(fileInput.files[0].name)}`;
        } else if (haveText) {
            mbConfigPayload = { text: textArea.value };
        } else {
            mbConfigPayload = null;
        }
    };

    fileInput.addEventListener('change', () => {
        if (errorEl) errorEl.style.display = 'none';
        if (textArea) textArea.value = '';  // file wins
        updateButtonState();
        _detectMediaBrowserProvider();
    });

    if (textArea) {
        // Debounced: the operator is typing/pasting a multi-line config, and
        // we don't want a detect round-trip per keystroke.
        let textDebounce = null;
        textArea.addEventListener('input', () => {
            if (errorEl) errorEl.style.display = 'none';
            if (textArea.value.trim()) {
                fileInput.value = '';
                if (dropLabel) dropLabel.textContent = 'Drop a .conf file here, or click to pick';
            }
            updateButtonState();
            if (textDebounce) clearTimeout(textDebounce);
            textDebounce = setTimeout(_detectMediaBrowserProvider, 600);
        });
    }

    if (dropZone) {
        ['dragenter', 'dragover'].forEach(ev => dropZone.addEventListener(ev, e => {
            e.preventDefault();
            e.stopPropagation();
            dropZone.style.borderColor = 'var(--accent)';
        }));
        ['dragleave', 'drop'].forEach(ev => dropZone.addEventListener(ev, e => {
            e.preventDefault();
            e.stopPropagation();
            dropZone.style.borderColor = '';
        }));
        dropZone.addEventListener('drop', e => {
            const files = e.dataTransfer && e.dataTransfer.files;
            if (files && files.length > 0) {
                fileInput.files = files;
                fileInput.dispatchEvent(new Event('change'));
            }
        });
    }
}

// Ask the backend which provider the supplied config looks like, and
// preselect it. Detection is a convenience, never a lock-in — the operator
// can always override the choice before hitting Configure.
//
// This only parses; nothing is written to the Pi until Configure is pressed.
async function _detectMediaBrowserProvider() {
    const row = document.getElementById('mbProviderRow');
    const select = document.getElementById('mbProviderSelect');
    if (!row || !select || !currentDevice || !mbConfigPayload) return;

    const formData = new FormData();
    if (mbConfigPayload.file) formData.append('file', mbConfigPayload.file);
    else if (mbConfigPayload.text) formData.append('config_text', mbConfigPayload.text);
    else return;

    try {
        await fetchCsrfToken();
        const response = await fetch(
            `${currentDevice.url}/admin/media-browser/detect-provider`, {
                method: 'POST',
                headers: { 'X-CSRF-Token': csrfToken || '' },
                body: formData,
            });
        const result = await response.json();
        if (!result.ok) {
            // An unparseable config is reported when they press Configure;
            // don't nag mid-typing. Just leave the picker hidden.
            row.style.display = 'none';
            return;
        }

        const d = result.data;
        select.innerHTML = (d.choices || []).map(c =>
            `<option value="${escapeHtml(c.value)}"${c.value === d.provider ? ' selected' : ''}>${escapeHtml(c.label)}</option>`
        ).join('');
        row.style.display = '';
        _renderProviderNote(d.detected_provider);
        select.onchange = () => _renderProviderNote(d.detected_provider);
    } catch (e) {
        console.warn('Provider detection failed (falling back to custom):', e);
        row.style.display = 'none';
    }
}

function _renderProviderNote(detected) {
    const select = document.getElementById('mbProviderSelect');
    const note = document.getElementById('mbProviderNote');
    if (!select || !note) return;

    const chosen = select.value;
    const label = select.options[select.selectedIndex]
        ? select.options[select.selectedIndex].text : chosen;

    const parts = [];
    if (detected) {
        parts.push(`Detected <strong>${escapeHtml(detected)}</strong> from the config.`);
    } else {
        parts.push('Could not identify the provider from this config — '
            + 'that is fine, the default below works with any standard '
            + 'WireGuard file.');
    }

    // Be straight about the trade. Port forwarding is the only thing riding
    // on this choice, and pretending otherwise is how a customer ends up
    // thinking their box is broken when it is working exactly as it can.
    if (chosen === 'protonvpn') {
        parts.push('ProtonVPN mode also forwards a port, which helps torrents '
            + 'start faster. Generate the config with NAT-PMP enabled.');
    } else if (chosen === 'custom') {
        parts.push('Runs your config exactly as written. No port forwarding — '
            + 'downloads work, some torrents just take longer to get going.');
    } else {
        parts.push(`Lets Gluetun pick ${escapeHtml(label)} servers for you. `
            + 'This provider does not offer port forwarding.');
    }
    note.innerHTML = parts.join(' ');
}

async function configureMediaBrowser() {
    if (!currentDevice) {
        showNotification('Not connected to a device', 'error');
        return;
    }
    if (!mbConfigPayload) {
        showNotification('Drop a WireGuard .conf or paste config text first', 'warning');
        return;
    }

    const button = document.getElementById('mbConfigureBtn');
    const errorEl = document.getElementById('mbConfigError');
    if (button) button.disabled = true;
    if (errorEl) errorEl.style.display = 'none';

    // Make sure we have a fresh CSRF token
    await fetchCsrfToken();

    const formData = new FormData();
    if (mbConfigPayload.file) {
        formData.append('file', mbConfigPayload.file);
    } else if (mbConfigPayload.text) {
        formData.append('config_text', mbConfigPayload.text);
    }
    // Whatever the operator settled on. Absent (detection never ran) means
    // the backend detects for itself and falls back to `custom`.
    const providerSelect = document.getElementById('mbProviderSelect');
    if (providerSelect && providerSelect.value) {
        formData.append('provider', providerSelect.value);
    }

    try {
        const response = await fetch(`${currentDevice.url}/admin/media-browser/setup`, {
            method: 'POST',
            headers: { 'X-CSRF-Token': csrfToken || '' },
            body: formData,
        });
        const result = await response.json();
        if (!result.ok) {
            throw new Error(result.error?.message || 'Setup failed');
        }
        mbActiveJobId = result.data.job_id;
        mbSetupStartTs = Date.now();
        mbForceSetupView = false;
        _mbShowState('configuring');
        _startSetupPolling();
    } catch (e) {
        console.error('Media Browser setup failed:', e);
        if (errorEl) {
            errorEl.style.display = 'block';
            errorEl.textContent = e.message || String(e);
        }
        if (button) button.disabled = false;
    }
}

function _startSetupPolling() {
    if (mbSetupPollHandle !== null) {
        clearInterval(mbSetupPollHandle);
    }
    const tick = async () => {
        if (!currentDevice || !mbActiveJobId) return;
        try {
            const data = await apiGet(`${currentDevice.url}/admin/media-browser/setup-status/${mbActiveJobId}`);
            _renderSetupStatus(data);
            if (data.status === 'success' || data.status === 'failed' || data.status === 'unknown') {
                clearInterval(mbSetupPollHandle);
                mbSetupPollHandle = null;
                if (data.status === 'success') {
                    showNotification('Media Browser configured successfully!', 'success');
                    mbActiveJobId = null;
                    setTimeout(() => refreshMediaBrowserStatus(), 1500);
                } else if (data.status === 'failed') {
                    _onSetupFailed(data);
                } else {
                    // status=unknown (server restart cleared the job) — fall back
                    mbActiveJobId = null;
                    refreshMediaBrowserStatus();
                }
            }
        } catch (e) {
            console.warn('Setup status poll failed (will retry):', e);
        }
    };
    // Kick off an immediate tick so the UI doesn't sit blank for 2 sec, then poll
    tick();
    mbSetupPollHandle = setInterval(tick, 2000);
}

function _renderSetupStatus(data) {
    const labelEl = document.getElementById('mbConfiguringLabel');
    const progressEl = document.getElementById('mbConfiguringProgress');
    const elapsedEl = document.getElementById('mbConfiguringElapsed');
    const logEl = document.getElementById('mbLogTail');
    const statusBox = document.getElementById('mbConfiguringStatus');

    const elapsed = data.elapsed_sec || 0;
    if (elapsedEl) elapsedEl.textContent = `Elapsed: ${elapsed}s`;

    // Pull the most recent stdout line as the live label, fall back to "Working..."
    const lines = data.log_lines || [];
    const latest = lines.length ? lines[lines.length - 1] : '';
    if (labelEl) labelEl.textContent = latest.trim() || `Working… (${elapsed}s)`;

    // Crude progress bar — setup_services.sh runs ~90 sec; cap at 95% while
    // running so the bar doesn't claim done before the script actually exits.
    if (progressEl) {
        let pct = Math.min(95, Math.round((elapsed / 90) * 95));
        if (data.status === 'success') pct = 100;
        if (data.status === 'failed')  pct = 100;
        progressEl.style.width = `${pct}%`;
    }

    if (logEl) {
        logEl.textContent = lines.join('\n');
        logEl.scrollTop = logEl.scrollHeight;
    }

    if (statusBox) {
        statusBox.classList.remove('complete', 'error');
        if (data.status === 'success') statusBox.classList.add('complete');
        if (data.status === 'failed')  statusBox.classList.add('error');
    }
}

function _onSetupFailed(data) {
    mbForceSetupView = true;
    mbActiveJobId = null;
    const tail = (data.log_lines || []).slice(-5).join('\n');
    showNotification(`Media Browser setup failed (exit ${data.exit_code}). See log for details.`, 'error', 8000);

    // Render State A again with the failure surfaced as an inline error
    _mbShowState('notconfigured');
    _setupMediaBrowserDropZone();
    const errorEl = document.getElementById('mbConfigError');
    if (errorEl) {
        errorEl.style.display = 'block';
        errorEl.innerHTML = `<strong>Setup failed (exit ${escapeHtml(String(data.exit_code))})</strong><br><pre style="margin-top: 0.5rem; font-size: 0.75rem; white-space: pre-wrap;">${escapeHtml(tail)}</pre>`;
    }
    const button = document.getElementById('mbConfigureBtn');
    if (button) button.disabled = false;
}

let _reconfArmTimer = null;
function reconfigureMediaBrowser(ev) {
    // Two-tap inline confirm instead of window.confirm(): iOS home-screen
    // web apps can silently suppress native dialogs, which made this
    // button appear completely dead in the field — on the box's one
    // owner-accessible repair flow. First tap arms (button changes text),
    // second tap within 6s proceeds, timeout disarms.
    const btn = ev && ev.target ? ev.target.closest('button') : null;
    if (btn && btn.dataset.armed !== '1') {
        btn.dataset.armed = '1';
        btn.dataset.origLabel = btn.innerHTML;
        btn.innerHTML = 'Tap again to confirm — services will be rebuilt';
        clearTimeout(_reconfArmTimer);
        _reconfArmTimer = setTimeout(() => {
            btn.dataset.armed = '0';
            btn.innerHTML = btn.dataset.origLabel;
        }, 6000);
        return;
    }
    if (btn) {
        clearTimeout(_reconfArmTimer);
        btn.dataset.armed = '0';
        btn.innerHTML = btn.dataset.origLabel || btn.innerHTML;
    }
    mbForceSetupView = true;
    mbActiveJobId = null;
    if (mbSetupPollHandle !== null) {
        clearInterval(mbSetupPollHandle);
        mbSetupPollHandle = null;
    }
    _mbShowState('notconfigured');
    _setupMediaBrowserDropZone();
}

// ===== MEDIA BROWSER VISIBILITY GATE =====
//
// The Media Browser tab is hidden by default in index.html (display: none on
// the tab nav button + bottom-nav button + tab section). It's only revealed
// when /admin/media-browser/visibility returns visible:true, which mirrors the
// kiosk's persisted media_browser_unlocked flag set via the secret sequence.
// The backend ALSO enforces this server-side — every other /admin/media-browser/*
// endpoint returns 403 when the flag is false — so this hide is purely UX
// polish to keep the feature invisible to users-not-in-the-know.

async function checkMediaBrowserVisibility() {
    if (!currentDevice) return false;

    let visible = false;
    try {
        const response = await fetch(`${currentDevice.url}/admin/media-browser/visibility`);
        const data = await response.json();
        visible = !!(data && data.ok && data.data && data.data.visible === true);
        // Layer 2 surface: tracked for any future UI that wants to badge
        // the tab nav when unlocked-but-unconfigured. The existing
        // dashboard already detects `configured: false` from /status and
        // shows a Configure VPN form, so no DOM change is required here
        // today.
        window.mbVpnConfigured = !!(data && data.ok && data.data && data.data.vpn_configured === true);
    } catch (e) {
        console.warn('Media Browser visibility check failed (treating as locked):', e);
        visible = false;
    }

    const tabBtn = document.getElementById('mbTabNavBtn');
    const bottomNavBtn = document.getElementById('mbBottomNavBtn');
    const tabSection = document.getElementById('mediabrowser');

    if (visible) {
        if (tabBtn) tabBtn.style.display = '';
        if (bottomNavBtn) bottomNavBtn.style.display = '';
        if (tabSection) tabSection.style.display = '';
    } else {
        if (tabBtn) tabBtn.style.display = 'none';
        if (bottomNavBtn) bottomNavBtn.style.display = 'none';
        if (tabSection) {
            tabSection.style.display = 'none';
            tabSection.classList.remove('active');
        }
        // If the user happened to be on the MB tab when the device disconnected
        // / locked, bounce them back to Videos.
        if (tabBtn && tabBtn.classList.contains('active')) {
            const videosTabBtn = document.querySelector('.tab[data-tab="videos"]');
            if (videosTabBtn) videosTabBtn.click();
        }
    }

    return visible;
}

// ===== MEDIA BROWSER STATE C ENRICHMENT =====
//
// Health summary, credentials expander, restart, reset — all gated by the
// same visibility check. Frontend lazy-fetches credentials only when the
// expander is opened (so creds don't leak into routine traffic) and the
// health summary is manual-refresh only.

let mbCredentialsLoaded = false;     // cache flag — re-fetch on each open
let mbResetModalWired = false;
let mbCredentialsExpanderWired = false;

async function refreshMediaBrowserHealthSummary() {
    if (!currentDevice) return;
    const target = document.getElementById('mbHealthSummary');
    const button = document.getElementById('mbHealthRefreshBtn');
    if (!target) return;

    target.innerHTML = '<span class="loading">Loading…</span>';
    if (button) button.disabled = true;

    try {
        const data = await apiGet(`${currentDevice.url}/admin/media-browser/health-summary`);
        target.innerHTML = _renderHealthSummaryRows(data);
    } catch (e) {
        console.error('Health summary fetch failed:', e);
        target.innerHTML = `<span class="loading" style="color: var(--error);">Failed to load: ${escapeHtml(e.message || String(e))}</span>`;
    } finally {
        if (button) button.disabled = false;
    }
}

function _renderHealthSummaryRows(d) {
    const fmt = (v, fallback = '—') => (v === -1 || v === null || v === undefined) ? fallback : v;

    // Library
    const libRow = `
        <div class="health-row">
            <span class="health-label">Library</span>
            <span class="health-value">${escapeHtml(String(fmt(d.library_count, 'unavailable')))}${d.library_count >= 0 ? ' movies' : ''}</span>
        </div>`;

    // Queue (active downloads + speed)
    let queueText = 'unavailable';
    if (d.queue_count >= 0) {
        const speedSuffix = d.queue_active_dl_mbps > 0 ? ` (${d.queue_active_dl_mbps.toFixed(1)} Mbps)` : '';
        queueText = `${d.queue_count} active${speedSuffix}`;
    }
    const queueRow = `
        <div class="health-row">
            <span class="health-label">Queue</span>
            <span class="health-value">${escapeHtml(queueText)}</span>
        </div>`;

    // qBittorrent torrents
    let qbitText = 'unavailable';
    if (d.qbit_active_torrents >= 0) {
        qbitText = `${d.qbit_active_torrents} torrents`;
        if (d.qbit_seeding_torrents >= 0) qbitText += ` (${d.qbit_seeding_torrents} seeding)`;
    }
    const qbitRow = `
        <div class="health-row">
            <span class="health-label">qBittorrent</span>
            <span class="health-value">${escapeHtml(qbitText)}</span>
        </div>`;

    // VPN port.
    //
    // "not supported by this VPN" is a normal, healthy state — most
    // providers simply do not offer port forwarding, and gluetun only
    // implements it for a handful. Showing that in red as "unavailable"
    // (which is what this did for every non-ProtonVPN box) reads as a fault
    // and sends the operator looking for a broken thing that isn't there.
    // Only a port we EXPECTED and did not get is an error.
    let portColor = 'var(--text-primary)';
    let portText = 'unavailable';
    let portTitle = '';
    if (d.vpn_port_status === 'synced') {
        portColor = 'var(--success)';
        portText = `${d.vpn_forwarded_port} ✓ synced`;
    } else if (d.vpn_port_status === 'drift') {
        portColor = 'var(--accent)';
        portText = `${d.vpn_forwarded_port} ⚠ drift`;
    } else if (d.vpn_port_status === 'unsupported') {
        portColor = 'var(--text-secondary)';
        portText = 'not supported by this VPN';
        portTitle = 'This VPN provider does not offer port forwarding. '
            + 'Downloads still work; incoming peer connections are limited, '
            + 'so some torrents may be slower to start.';
    } else {
        portColor = 'var(--error)';
        portText = 'unavailable';
        portTitle = 'This VPN supports port forwarding but no port is '
            + 'currently leased.';
    }
    const portRow = `
        <div class="health-row">
            <span class="health-label">VPN port</span>
            <span class="health-value" style="color: ${portColor};"${portTitle ? ` title="${escapeHtml(portTitle)}"` : ''}>${escapeHtml(portText)}</span>
        </div>`;

    return libRow + queueRow + qbitRow + portRow;
}

// ----- Credentials expander (lazy-loaded) -----

function _wireMediaBrowserCredentialsExpander() {
    if (mbCredentialsExpanderWired) return;
    const expander = document.getElementById('mbCredentialsExpander');
    if (!expander) return;
    expander.addEventListener('toggle', () => {
        if (expander.open && !mbCredentialsLoaded) {
            _loadMediaBrowserCredentials();
        }
    });
    mbCredentialsExpanderWired = true;
}

async function _loadMediaBrowserCredentials() {
    if (!currentDevice) return;
    const body = document.getElementById('mbCredentialsBody');
    if (!body) return;

    body.innerHTML = '<span class="loading">Loading credentials…</span>';

    try {
        const response = await fetch(`${currentDevice.url}/admin/media-browser/credentials`);
        const result = await response.json();
        if (!result.ok) {
            const msg = result.error?.code === 'credentials_not_ready'
                ? 'Setup not yet complete — credentials not yet available.'
                : (result.error?.message || 'Failed to load credentials');
            body.innerHTML = `<span class="loading" style="color: var(--accent);">${escapeHtml(msg)}</span>`;
            return;
        }
        const data = result.data || {};
        body.innerHTML = _renderCredentialsHTML(data);
        // Wire up Show + Copy buttons after they're in the DOM
        body.querySelectorAll('[data-mb-toggle]').forEach(btn => {
            btn.addEventListener('click', () => _toggleCredentialReveal(btn));
        });
        body.querySelectorAll('[data-mb-copy]').forEach(btn => {
            btn.addEventListener('click', () => _copyCredentialValue(btn));
        });
        mbCredentialsLoaded = true;
    } catch (e) {
        console.error('Credentials load failed:', e);
        body.innerHTML = `<span class="loading" style="color: var(--error);">Failed to load: ${escapeHtml(e.message || String(e))}</span>`;
    }
}

function _renderCredentialsHTML(d) {
    // Each value is masked with a placeholder; the real value lives in
    // data-mb-value on the row so [Show] / [Copy] can read it without
    // putting it in the rendered DOM until the operator asks.
    const row = (label, url, value, idSuffix, showCopyOnly = false) => {
        const safeValue = String(value || '');
        return `
        <div style="margin-bottom: 1rem; padding: 0.75rem; background: var(--bg-dark); border: 1px solid var(--border-color); border-radius: 4px;">
            <div style="font-weight: 600; color: var(--accent2); margin-bottom: 0.25rem;">${escapeHtml(label)}</div>
            ${url ? `<div style="font-size: 0.8rem; color: var(--text-secondary); margin-bottom: 0.5rem;"><code>${escapeHtml(url)}</code></div>` : ''}
            <div style="display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; font-family: monospace;">
                <span id="mbCred${idSuffix}Display" style="flex: 1 1 200px; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${'•'.repeat(Math.min(20, Math.max(8, safeValue.length)))}</span>
                ${showCopyOnly ? '' : `<button class="btn-secondary small" data-mb-toggle="${idSuffix}" data-mb-value="${escapeHtml(safeValue)}" data-mb-revealed="0">${spriteIcon('ic-eye')} Show</button>`}
                <button class="btn-secondary small" data-mb-copy="1" data-mb-value="${escapeHtml(safeValue)}">${spriteIcon('ic-copy')} Copy</button>
            </div>
        </div>`;
    };

    const username = d.qbittorrent_admin_username || 'admin';
    return [
        // API key VALUES are no longer sent by the backend. The Content Manager
        // is unauthenticated on the LAN, so returning them disclosed them to
        // any device on the customer's network. Show the URL and point at the
        // box — anyone who can SSH can already read services/.env.
        row('Radarr', d.radarr_url || 'http://localhost:7878', null, 'Radarr'),
        row('Prowlarr', d.prowlarr_url || 'http://localhost:9696', null, 'Prowlarr'),
        // qBit username is non-secret — show it directly
        `<div style="margin-bottom: 1rem; padding: 0.75rem; background: var(--bg-dark); border: 1px solid var(--border-color); border-radius: 4px;">
            <div style="font-weight: 600; color: var(--accent2); margin-bottom: 0.25rem;">qBittorrent</div>
            <div style="font-size: 0.8rem; color: var(--text-secondary); margin-bottom: 0.5rem;"><code>http://localhost:8080  (via SSH tunnel)</code></div>
            <div style="display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; font-family: monospace; margin-bottom: 0.5rem;">
                <span style="flex: 1 1 200px;">Username: <strong>${escapeHtml(username)}</strong></span>
                <button class="btn-secondary small" data-mb-copy="1" data-mb-value="${escapeHtml(username)}">${spriteIcon('ic-copy')} Copy</button>
            </div>
            <div style="display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; font-family: monospace;">
                <span style="flex: 1 1 320px; opacity: .8;">Password: read it on the box &mdash;
                    <code>sudo cat /opt/magic_dingus_box/services/.env</code></span>
            </div>
        </div>`,
    ].join('');
}

function _toggleCredentialReveal(btn) {
    const idSuffix = btn.dataset.mbToggle;
    const display = document.getElementById(`mbCred${idSuffix}Display`);
    const revealed = btn.dataset.mbRevealed === '1';
    if (!display) return;
    if (revealed) {
        const len = (btn.dataset.mbValue || '').length;
        display.textContent = '•'.repeat(Math.min(20, Math.max(8, len)));
        btn.innerHTML = `${spriteIcon('ic-eye')} Show`;
        btn.dataset.mbRevealed = '0';
    } else {
        display.textContent = btn.dataset.mbValue || '';
        btn.innerHTML = `${spriteIcon('ic-eye-off')} Hide`;
        btn.dataset.mbRevealed = '1';
    }
}

async function _copyCredentialValue(btn) {
    const value = btn.dataset.mbValue || '';
    if (!value) return;
    try {
        await navigator.clipboard.writeText(value);
    } catch (e) {
        // Fallback for browsers without clipboard API (rare in CM context)
        const tmp = document.createElement('textarea');
        tmp.value = value;
        document.body.appendChild(tmp);
        tmp.select();
        try { document.execCommand('copy'); } catch (_) {}
        document.body.removeChild(tmp);
    }
    const original = btn.textContent;
    btn.textContent = '✓ Copied';
    btn.disabled = true;
    setTimeout(() => {
        btn.textContent = original;
        btn.disabled = false;
    }, 1200);
}

// ----- Restart -----

async function restartMediaBrowserServices() {
    if (!currentDevice) return;
    const button = document.getElementById('mbRestartBtn');
    const status = document.getElementById('mbActionStatus');
    if (!button) return;

    const originalLabel = button.textContent;
    button.disabled = true;
    button.textContent = 'Restarting…';
    if (status) {
        status.style.display = 'block';
        status.style.color = 'var(--text-secondary)';
        status.textContent = 'Restarting services (this can take ~30 seconds)…';
    }

    await fetchCsrfToken();

    try {
        const response = await fetch(`${currentDevice.url}/admin/media-browser/restart`, {
            method: 'POST',
            headers: { 'X-CSRF-Token': csrfToken || '' },
        });
        const result = await response.json();
        if (!result.ok) {
            throw new Error(result.error?.message || 'Restart failed');
        }
        if (status) {
            status.style.color = 'var(--success)';
            status.textContent = '✓ Services restarted';
        }
        showNotification('Services restarted', 'success');
        // Wait a beat, then re-fetch /status so the container table reflects
        // the new "Up 0 minutes" / "starting" states.
        setTimeout(() => refreshMediaBrowserStatus(), 2000);
    } catch (e) {
        console.error('Restart failed:', e);
        if (status) {
            status.style.color = 'var(--error)';
            status.textContent = `Restart failed: ${e.message || e}`;
        }
        showNotification(`Restart failed: ${e.message || e}`, 'error');
    } finally {
        button.disabled = false;
        button.textContent = originalLabel;
    }
}

// ----- Reset (with confirmation modal) -----

function openMediaBrowserResetModal() {
    _wireMediaBrowserResetModal();
    const modal = document.getElementById('mbResetModal');
    const input = document.getElementById('mbResetConfirmInput');
    const confirmBtn = document.getElementById('mbResetConfirmBtn');
    if (!modal) return;
    if (input) input.value = '';
    if (confirmBtn) confirmBtn.disabled = true;
    modal.classList.add('active');
    if (input) setTimeout(() => input.focus(), 50);
}

function closeMediaBrowserResetModal() {
    const modal = document.getElementById('mbResetModal');
    if (modal) modal.classList.remove('active');
}

function _wireMediaBrowserResetModal() {
    if (mbResetModalWired) return;
    const cancelBtn = document.getElementById('mbResetCancelBtn');
    const confirmBtn = document.getElementById('mbResetConfirmBtn');
    const input = document.getElementById('mbResetConfirmInput');
    if (cancelBtn) cancelBtn.addEventListener('click', closeMediaBrowserResetModal);
    if (input) {
        input.addEventListener('input', () => {
            if (confirmBtn) confirmBtn.disabled = (input.value.trim().toUpperCase() !== 'RESET');
        });
    }
    if (confirmBtn) {
        confirmBtn.addEventListener('click', async () => {
            await _confirmMediaBrowserReset();
        });
    }
    mbResetModalWired = true;
}

async function _confirmMediaBrowserReset() {
    if (!currentDevice) return;
    const confirmBtn = document.getElementById('mbResetConfirmBtn');
    const cancelBtn = document.getElementById('mbResetCancelBtn');
    const status = document.getElementById('mbActionStatus');

    if (confirmBtn) {
        confirmBtn.disabled = true;
        confirmBtn.textContent = 'Resetting…';
    }
    if (cancelBtn) cancelBtn.disabled = true;

    await fetchCsrfToken();

    try {
        const response = await fetch(`${currentDevice.url}/admin/media-browser/reset`, {
            method: 'POST',
            headers: {
                'X-CSRF-Token': csrfToken || '',
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({ confirm: 'RESET' }),
        });
        const result = await response.json();
        if (!result.ok) {
            throw new Error(result.error?.message || 'Reset failed');
        }
        showNotification('Media Browser reset', 'success');
        if (status) {
            status.style.display = 'block';
            status.style.color = 'var(--success)';
            status.textContent = '✓ Reset complete — returning to setup screen';
        }
        closeMediaBrowserResetModal();
        // Force a fresh status fetch so the frontend transitions back to
        // State A (drop-zone). mbCredentialsLoaded reset so the next setup
        // doesn't show stale creds.
        mbCredentialsLoaded = false;
        mbForceSetupView = false;
        setTimeout(() => refreshMediaBrowserStatus(), 800);
    } catch (e) {
        console.error('Reset failed:', e);
        showNotification(`Reset failed: ${e.message || e}`, 'error');
        if (status) {
            status.style.display = 'block';
            status.style.color = 'var(--error)';
            status.textContent = `Reset failed: ${e.message || e}`;
        }
    } finally {
        if (confirmBtn) {
            confirmBtn.disabled = false;
            confirmBtn.textContent = 'Confirm Reset';
        }
        if (cancelBtn) cancelBtn.disabled = false;
    }
}

// Wire State C interactions when DOM is ready (idempotent — uses internal flags)
document.addEventListener('DOMContentLoaded', () => {
    _wireMediaBrowserCredentialsExpander();
    _wireMediaBrowserResetModal();
});
