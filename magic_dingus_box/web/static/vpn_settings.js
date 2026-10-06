// VPN server country (Media Browser > Advanced).
//
// Self-contained on purpose, so the planned split of manager.js does not
// have to carry it: it only uses manager.js's shared globals (currentDevice,
// apiGet, apiPost, fetchCsrfToken, csrfToken, escapeHtml)
// and wires itself to the #mbVpnAdvanced <details> in index.html. The
// backend (vpn_settings.py) validates the country, rewrites only the
// VPN_COUNTRIES line of services/.env and recreates only the VPN container.
//
// Loaded lazily: nothing is fetched until the operator opens "Advanced".
(function () {
    'use strict';

    let pollTimer = null;

    function el(id) { return document.getElementById(id); }

    function setStatus(html, tone) {
        const box = el('mbVpnCountryStatus');
        if (!box) return;
        if (!html) { box.style.display = 'none'; box.innerHTML = ''; return; }
        box.style.display = 'block';
        box.className = 'restore-status' + (tone ? ' ' + tone : '');
        box.innerHTML = html;
    }

    function render(data) {
        const select = el('mbVpnCountrySelect');
        const apply = el('mbVpnCountryApply');
        const note = el('mbVpnCountryNote');
        if (!select || !apply) return;

        const current = data.country || '';
        const choices = Array.isArray(data.choices) ? data.choices.slice() : [];
        // A hand-edited value outside the list is still shown truthfully.
        if (current && !choices.includes(current)) choices.unshift(current);
        select.innerHTML = '';
        if (!current) {
            const any = document.createElement('option');
            any.value = '';
            any.textContent = 'Any country (not set)';
            select.appendChild(any);
        }
        choices.forEach(c => {
            const opt = document.createElement('option');
            opt.value = c;
            opt.textContent = c + (c === data.default ? ' (default)' : '');
            select.appendChild(opt);
        });
        select.value = current;
        select.dataset.current = current;

        const running = data.job && data.job.status === 'running';
        select.disabled = !data.editable || running;
        apply.disabled = true;
        if (!data.editable && note) {
            note.textContent = data.reason || 'The VPN country cannot be changed on this box.';
        }

        if (running) {
            setStatus('<span class="loading">Reconnecting the VPN — downloads pause for about a minute…</span>');
            schedulePoll();
        } else if (data.job && data.job.status === 'success') {
            const tail = (data.job.log || []).slice(-1)[0] || 'VPN reconnected.';
            setStatus(escapeHtml(tail.replace(/^\[vpn-country\]\s*/, '')), 'success');
        } else if (data.job && data.job.status === 'failed') {
            const tail = (data.job.log || []).slice(-1)[0] || '';
            setStatus('The VPN did not come back yet. ' + escapeHtml(tail.replace(/^\[vpn-country\]\s*/, '')) +
                      ' It keeps retrying on its own; the Box health check shows how it is doing.', 'error');
        }
    }

    async function load() {
        if (typeof currentDevice === 'undefined' || !currentDevice) return;
        try {
            render(await apiGet(`${currentDevice.url}/admin/media-browser/vpn-country`));
        } catch (e) {
            setStatus('Could not read the VPN country: ' + escapeHtml(e.message || String(e)), 'error');
        }
    }

    function schedulePoll() {
        if (pollTimer) return;
        pollTimer = setTimeout(async () => {
            pollTimer = null;
            const details = el('mbVpnAdvanced');
            if (details && details.open) await load();
        }, 3000);
    }

    async function apply() {
        const select = el('mbVpnCountrySelect');
        const btn = el('mbVpnCountryApply');
        if (!select || !btn || !currentDevice) return;
        const country = select.value;
        if (!country || country === select.dataset.current) return;
        // Two-tap inline confirm (iOS home-screen apps can suppress
        // window.confirm — the same reason Reconfigure does this).
        if (btn.dataset.armed !== '1') {
            btn.dataset.armed = '1';
            btn.textContent = `Reconnect to ${country}?`;
            setTimeout(() => { btn.dataset.armed = ''; btn.textContent = 'Apply'; }, 6000);
            return;
        }
        btn.dataset.armed = '';
        btn.textContent = 'Apply';
        btn.disabled = true;
        select.disabled = true;
        try {
            if (!csrfToken) await fetchCsrfToken();
            await apiPost(`${currentDevice.url}/admin/media-browser/vpn-country`, { country });
            setStatus('<span class="loading">Reconnecting the VPN — downloads pause for about a minute…</span>');
            await load();
        } catch (e) {
            setStatus('Could not change the VPN country: ' + escapeHtml(e.message || String(e)), 'error');
            select.disabled = false;
            btn.disabled = select.value === select.dataset.current;
        }
    }

    function wire() {
        const details = el('mbVpnAdvanced');
        const select = el('mbVpnCountrySelect');
        const btn = el('mbVpnCountryApply');
        if (!details || details.dataset.wired === '1') return;
        details.dataset.wired = '1';
        // Re-read on every open: cheap, and always right after a device switch.
        details.addEventListener('toggle', () => { if (details.open) load(); });
        if (select) {
            select.addEventListener('change', () => {
                if (btn) btn.disabled = !select.value || select.value === select.dataset.current;
                setStatus('');
            });
        }
        if (btn) btn.addEventListener('click', apply);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', wire);
    } else {
        wire();
    }
})();
