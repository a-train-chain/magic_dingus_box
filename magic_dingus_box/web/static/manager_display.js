/**
 * CRT Look tab — live tuning of the kiosk's Tube CRT engine.
 *
 * Server: admin_display.py. GET /admin/display/tube answers what the kiosk
 * holds (`?state_only=1` leaves out the row file); POST takes {ops: [...]}
 * and answers the top sequence number it assigned. The kiosk reports the
 * newest number it has applied as `handled_seq`; a change is confirmed when
 * that reaches the number its POST was given.
 *
 * Everything the page draws (labels, hints, groups, ranges, masks, looks,
 * engines) comes from the row file — nothing is hard-coded here.
 *
 * Loads LAST, after vpn_settings.js. Nothing in this file runs while it
 * loads: only declarations. The logic (tubeAddOp, tubeShown, tubeMovedKeys,
 * tubeConfirm, makeOpSender) touches no DOM and is run under node by
 * tests/test_manager_display_js.py; the DOM half below it is kept thin.
 */

// ===== PURE LOGIC (no DOM; tested) =====

/** Add `op` to the unsent list: the newest value per row wins, one degauss is enough. */
function tubeAddOp(unsent, op) {
    if (op.op === 'row') {
        return unsent.filter(o => !(o.op === 'row' && o.key === op.key)).concat([op]);
    }
    if (op.op === 'degauss' && unsent.some(o => o.op === 'degauss')) {
        return unsent;
    }
    return unsent.concat([op]);
}

/** The kiosk's state with `ops` (sent-but-unconfirmed, then unsent) applied on top. */
function tubeShown(kiosk, ops, rows) {
    const shown = {
        engine: kiosk.engine,
        look: kiosk.look,
        mask: kiosk.mask,
        values: Object.assign({}, kiosk.values),
    };
    const lookByKey = key => rows.looks.find(l => l.key === key);
    const takeLook = look => {
        if (!look) return;
        shown.mask = look.mask;
        shown.values = Object.assign({}, look.values);
    };
    for (const op of ops) {
        if (op.op === 'row') shown.values[op.key] = op.value;
        else if (op.op === 'mask') shown.mask = op.mask;
        else if (op.op === 'engine') shown.engine = op.engine;
        else if (op.op === 'look') { shown.look = op.look; takeLook(lookByKey(op.look)); }
        else if (op.op === 'reset') takeLook(lookByKey(shown.look));
    }
    return shown;
}

/** Keys (rows, and 'mask') whose shown value differs from the shown look's. */
function tubeMovedKeys(shown, rows) {
    const look = rows.looks.find(l => l.key === shown.look);
    if (!look) return [];
    const moved = Object.keys(look.values).filter(k => shown.values[k] !== look.values[k]);
    if (shown.mask !== look.mask) moved.push('mask');
    return moved;
}

/** Drop the batches the kiosk has handled; a handled_seq that went backwards is a fresh kiosk. */
function tubeConfirm(sent, handledSeq, lastHandled) {
    if (handledSeq < lastHandled) return [];
    return sent.filter(b => b.seq > handledSeq);
}

/**
 * One POST in flight at a time. `post(ops)` resolves to {seq} or rejects with
 * an error carrying `code` (apiPost's errors). Ops added while a POST is out
 * are coalesced and sent together when it returns. A refused batch
 * (VALIDATION_ERROR) is dropped and `onRefused(message)` told; any other
 * failure (network, 5xx, TUBE_UNAVAILABLE, TUBE_BACKLOG) puts the batch back
 * in front of what arrived meanwhile and waits for the next pump() — which
 * onStatus() does on every poll. pending() is what the page draws on top of
 * the kiosk's state, so a value never snaps back while its POST is out.
 */
function makeOpSender(post, onRefused) {
    let unsent = [];
    let sent = [];
    let inFlight = null;  // the ops of the POST that has not answered yet
    let lastHandled = -1;

    function pump() {
        if (inFlight || unsent.length === 0) return;
        const ops = unsent;
        unsent = [];
        inFlight = ops;
        Promise.resolve()
            .then(() => post(ops))
            .then(result => {
                inFlight = null;
                sent = sent.concat([{ ops, seq: result.seq }]);
                pump();
            }, err => {
                inFlight = null;
                if (err && err.code === 'VALIDATION_ERROR') {
                    if (onRefused) onRefused(err.message || 'The TV refused that change');
                    return;
                }
                const meanwhile = unsent;
                unsent = ops;
                for (const op of meanwhile) unsent = tubeAddOp(unsent, op);
            });
    }

    return {
        add(op) { unsent = tubeAddOp(unsent, op); },
        pump,
        onStatus(handledSeq) {
            sent = tubeConfirm(sent, handledSeq, lastHandled);
            lastHandled = handledSeq;
            pump();
        },
        unsent() { return unsent.slice(); },
        sent() { return sent.slice(); },
        /** Every op the kiosk has not confirmed, oldest first: sent, in flight, unsent. */
        pending() {
            const ops = sent.reduce((all, b) => all.concat(b.ops), []);
            return ops.concat(inFlight || [], unsent);
        },
    };
}

// ===== DOM (thin) =====

const CRT_LOOK_POLL_MS = 1000;

let _crtLook = null;  // per device: see crtLookState()

function crtLookState() {
    const url = currentDevice ? currentDevice.url : null;
    if (!_crtLook || _crtLook.deviceUrl !== url) {
        if (_crtLook && _crtLook.timer) clearTimeout(_crtLook.timer);
        const st = {
            deviceUrl: url, rows: null, kiosk: null, available: false, answered: false,
            running: false, timer: null, held: null, els: null,
        };
        st.sender = makeOpSender(
            ops => apiPost(`${url}/admin/display/tube`, { ops }),
            msg => showNotification(msg, 'error'));
        _crtLook = st;
    }
    return _crtLook;
}

function crtLookTabUrl(stateOnly) {
    return `${currentDevice.url}/admin/display/tube${stateOnly ? '?state_only=1' : ''}`;
}

function setCrtLookTabShown(shown) {
    const tabBtn = document.getElementById('crtLookTabNavBtn');
    const bottomBtn = document.getElementById('crtLookBottomNavBtn');
    const section = document.getElementById('crtlook');
    [tabBtn, bottomBtn, section].forEach(el => { if (el) el.style.display = shown ? '' : 'none'; });
    if (shown) return;
    const wasOpen = section && section.classList.contains('active');
    if (section) section.classList.remove('active');
    if (wasOpen) {
        const videosTabBtn = document.querySelector('.tab[data-tab="videos"]');
        if (videosTabBtn) videosTabBtn.click();
    }
    stopCrtLookTab();
}

/** Show the tab only where the kiosk offers Tube. Called on every device change. */
async function checkCrtLookVisibility() {
    if (!currentDevice) return false;
    const st = crtLookState();
    let data = null;
    try {
        data = await apiGet(crtLookTabUrl(true));
    } catch (e) {
        data = null;
    }
    if (_crtLook !== st) return false;  // the device changed while we asked
    const available = !!(data && data.available === true);
    const section = document.getElementById('crtlook');
    const open = section && section.classList.contains('active');
    const kioskGone = !data || data.reason === 'kiosk_not_running';
    if (available) {
        setCrtLookTabShown(true);
        if (open) startCrtLookTab();  // a new device while the tab is open: poll it
    } else if (!(open && kioskGone)) setCrtLookTabShown(false);
    return available;
}

function startCrtLookTab() {
    if (!currentDevice) return;
    const st = crtLookState();
    if (st.running) return;
    st.running = true;
    renderCrtLook();
    crtLookPoll(st);
}

function stopCrtLookTab() {
    if (!_crtLook) return;
    _crtLook.running = false;
    if (_crtLook.timer) clearTimeout(_crtLook.timer);
    _crtLook.timer = null;
}

async function crtLookPoll(st) {
    st.timer = null;
    if (!st.running || _crtLook !== st) return;
    if (document.visibilityState === 'visible') {
        let data = null;
        try {
            data = await apiGet(crtLookTabUrl(!!st.rows));
        } catch (e) {
            data = null;  // the web admin itself is unreachable: same as a stopped kiosk
        }
        if (!st.running || _crtLook !== st) return;
        st.answered = true;
        if (data && data.available === true) {
            if (data.rows) st.rows = data.rows;
            st.kiosk = data.state;
            st.available = true;
            const handled = Number.isInteger(data.state.handled_seq) ? data.state.handled_seq : 0;
            st.sender.onStatus(handled);
        } else if (!data || data.reason === 'kiosk_not_running') {
            st.available = false;  // keep the tab and the unsent ops; stop sending
        } else {
            setCrtLookTabShown(false);
            return;
        }
        renderCrtLook();
    }
    if (st.running && _crtLook === st) st.timer = setTimeout(() => crtLookPoll(st), CRT_LOOK_POLL_MS);
}

function crtLookSend(op) {
    const st = _crtLook;
    if (!st || !st.available) return;
    st.sender.add(op);
    st.sender.pump();
    renderCrtLook();
}

function crtLookSelectStyle() {
    return 'padding: 0.5rem; background: var(--bg-dark); color: var(--text-primary); '
        + 'border: 1px solid var(--border-color); border-radius: 4px;';
}

function crtLookHintHtml(hint) {
    return (hint || []).map(line => escapeHtml(line)).join('<br>');
}

function buildCrtLook(st) {
    const rows = st.rows;
    const rowByKey = {};
    rows.rows.forEach(r => { rowByKey[r.key] = r; });
    const sel = crtLookSelectStyle();
    const desc = 'class="settings-description" style="margin: 0.25rem 0 0; color: var(--text-secondary); font-size: 0.85rem;"';

    const engineOptions = rows.engines.map(e =>
        `<option value="${escapeHtml(e.key)}">${escapeHtml(e.word)}</option>`).join('');
    const lookOptions = rows.looks.map(l =>
        `<option value="${escapeHtml(l.key)}">${escapeHtml(l.word)}</option>`).join('');
    const maskOptions = rows.mask.choices.map(c =>
        `<option value="${escapeHtml(c.key)}">${escapeHtml(c.word)}</option>`).join('');

    const rowHtml = key => {
        const isMask = key === 'mask';
        const row = isMask ? rows.mask : rowByKey[key];
        if (!row) return '';
        const control = isMask
            ? `<select data-tube-mask style="${sel} width: 100%;">${maskOptions}</select>`
            : `<div style="display: flex; gap: 0.75rem; align-items: center;">
                   <input type="range" data-tube-row="${escapeHtml(key)}" min="${rows.row_min}" max="${rows.row_max}" step="1"
                          aria-label="${escapeHtml(row.label)}" style="flex: 1 1 auto; min-width: 0;">
                   <span data-tube-num="${escapeHtml(key)}" style="min-width: 2.5ch; text-align: right; font-variant-numeric: tabular-nums;"></span>
               </div>`;
        const hint = isMask ? '' : crtLookHintHtml(row.hint);
        return `<div data-tube-line="${escapeHtml(key)}" style="padding: 0.6rem 0; border-top: 1px solid var(--bg-mid);">
                    <div style="display: flex; justify-content: space-between; gap: 0.5rem; margin-bottom: 0.3rem;">
                        <strong>${escapeHtml(row.label)}</strong>
                        <span data-tube-moved="${escapeHtml(key)}" style="color: var(--accent); font-size: 0.8rem;">moved</span>
                    </div>
                    ${control}
                    <p ${desc} data-tube-hint="${escapeHtml(key)}">${hint}</p>
                    <p ${desc} data-tube-na="${escapeHtml(key)}">a real tube draws this itself</p>
                </div>`;
    };

    const groups = rows.groups.map(g => `
        <div class="settings-section-mobile">
            <div class="section-title">${escapeHtml(g.title)}</div>
            <div class="section-body" style="padding-top: 0.25rem;">${g.rows.map(rowHtml).join('')}</div>
        </div>`).join('');

    document.getElementById('crtLookBody').innerHTML = `
        <div class="settings-section-mobile">
            <div class="section-title"><svg class="ic" aria-hidden="true"><use href="#ic-crt"/></svg> CRT Look</div>
            <div class="section-body">
                <label class="settings-description" for="crtLookEngine" style="display: block; margin-bottom: 0.35rem;">CRT Engine</label>
                <select id="crtLookEngine" style="${sel} width: 100%;">${engineOptions}</select>
                <p ${desc} id="crtLookEngineNote">The sliders apply when CRT Engine is Tube.</p>

                <label class="settings-description" for="crtLookLook" style="display: block; margin: 0.9rem 0 0.35rem;">Look</label>
                <select id="crtLookLook" style="${sel} width: 100%;">${lookOptions}</select>
                <p ${desc} id="crtLookLookHint"></p>

                <div style="display: flex; gap: 0.5rem; flex-wrap: wrap; margin-top: 0.9rem;">
                    <button class="btn-secondary small" id="crtLookReset">Reset to look</button>
                    <button class="btn-secondary small" id="crtLookDegauss">Degauss</button>
                </div>
                <p ${desc}>Degauss is sent to the TV now; its effect appears once the Tube renderer lands.</p>
            </div>
        </div>
        ${groups}`;

    const body = document.getElementById('crtLookBody');
    const els = {
        engine: document.getElementById('crtLookEngine'),
        engineNote: document.getElementById('crtLookEngineNote'),
        look: document.getElementById('crtLookLook'),
        lookHint: document.getElementById('crtLookLookHint'),
        reset: document.getElementById('crtLookReset'),
        degauss: document.getElementById('crtLookDegauss'),
        mask: body.querySelector('[data-tube-mask]'),
        ranges: {}, nums: {}, moved: {}, hints: {}, na: {},
    };
    body.querySelectorAll('[data-tube-row]').forEach(el => { els.ranges[el.dataset.tubeRow] = el; });
    body.querySelectorAll('[data-tube-num]').forEach(el => { els.nums[el.dataset.tubeNum] = el; });
    body.querySelectorAll('[data-tube-moved]').forEach(el => { els.moved[el.dataset.tubeMoved] = el; });
    body.querySelectorAll('[data-tube-hint]').forEach(el => { els.hints[el.dataset.tubeHint] = el; });
    body.querySelectorAll('[data-tube-na]').forEach(el => { els.na[el.dataset.tubeNa] = el; });

    els.engine.addEventListener('change', () => crtLookSend({ op: 'engine', engine: els.engine.value }));
    els.look.addEventListener('change', () => crtLookSend({ op: 'look', look: els.look.value }));
    els.reset.addEventListener('click', () => crtLookSend({ op: 'reset' }));
    els.degauss.addEventListener('click', () => crtLookSend({ op: 'degauss' }));
    if (els.mask) els.mask.addEventListener('change', () => crtLookSend({ op: 'mask', mask: els.mask.value }));

    Object.keys(els.ranges).forEach(key => {
        const range = els.ranges[key];
        const release = () => { if (st.held === range) st.held = null; };
        range.addEventListener('pointerdown', () => { st.held = range; });
        range.addEventListener('pointerup', release);
        range.addEventListener('pointercancel', release);
        range.addEventListener('change', release);
        range.addEventListener('blur', release);
        range.addEventListener('input', () => {
            const value = parseInt(range.value, 10);
            els.nums[key].textContent = String(value);
            crtLookSend({ op: 'row', key, value });
        });
    });
    st.els = els;
}

function renderCrtLook() {
    const st = _crtLook;
    const status = document.getElementById('crtLookStatus');
    if (!st || !status) return;
    status.textContent = st.available ? '' : (st.answered ? 'Waiting for the TV…' : 'Loading…');
    status.style.display = st.available ? 'none' : '';
    if (!st.rows || !st.kiosk) return;
    if (!st.els) buildCrtLook(st);
    const els = st.els;
    const rows = st.rows;
    const kiosk = st.kiosk;
    const waiting = !st.available;
    const shown = tubeShown(kiosk, st.sender.pending(), rows);
    const moved = tubeMovedKeys(shown, rows);
    const native = kiosk.crt_native === true;
    const rowByKey = {};
    rows.rows.forEach(r => { rowByKey[r.key] = r; });

    els.engine.value = shown.engine;
    els.engine.disabled = waiting;
    els.engineNote.style.display = shown.engine === 'tube' ? 'none' : '';
    els.look.value = shown.look;
    els.look.disabled = waiting;
    const look = rows.looks.find(l => l.key === shown.look);
    els.lookHint.innerHTML = look ? crtLookHintHtml(look.hint) : '';
    els.reset.disabled = waiting;
    els.degauss.disabled = waiting || kiosk.effective_engine !== 'tube';

    if (els.mask) {
        const maskNa = native && rows.mask.crt_native_neutral;
        els.mask.value = shown.mask;
        els.mask.disabled = waiting || maskNa;
        const choice = rows.mask.choices.find(c => c.key === shown.mask);
        els.hints.mask.innerHTML = choice ? crtLookHintHtml(choice.hint) : '';
        els.na.mask.style.display = maskNa ? '' : 'none';
        els.moved.mask.style.visibility = moved.includes('mask') ? 'visible' : 'hidden';
    }

    Object.keys(els.ranges).forEach(key => {
        const range = els.ranges[key];
        const na = native && !!(rowByKey[key] && rowByKey[key].crt_native_neutral);
        range.disabled = waiting || na;
        els.na[key].style.display = na ? '' : 'none';
        els.moved[key].style.visibility = moved.includes(key) ? 'visible' : 'hidden';
        if (st.held === range) return;  // never repaint a slider the pointer is holding
        const value = shown.values[key];
        if (typeof value === 'number') {
            range.value = String(value);
            els.nums[key].textContent = String(value);
        }
    });
}
