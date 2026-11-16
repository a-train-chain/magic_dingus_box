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

/** The server's limit on ops per POST (admin_display.MAX_OPS_PER_POST). */
const TUBE_MAX_OPS_PER_POST = 64;
/** A sent batch is called lost only on the kiosk's word: a status the kiosk
    wrote at least this many seconds after the server accepted the batch
    (status_ts vs accepted_at, both the box's clock) that still lacks its
    seq. A healthy kiosk confirms within a second; a status that is merely
    late (no fresh one at all) never trips it. */
const TUBE_CONFIRM_EVIDENCE_S = 4;
/** A POST with no answer by then is abandoned and retried (a transient failure). */
const TUBE_POST_TIMEOUT_MS = 10000;
/** Consecutive failed POSTs before the page says the TV is not being reached. */
const TUBE_FAILURES_BEFORE_NOTICE = 3;
/** Failures that clear by themselves and are retried on every poll; anything
    else (CSRF_ERROR, FORBIDDEN_HOST, CROSS_SITE_REQUEST, UNKNOWN_ERROR, ...)
    waits for the user. A network error or a non-JSON 5xx carries no code. */
const TUBE_TRANSIENT_CODES = ['TUBE_UNAVAILABLE', 'TUBE_BACKLOG', 'INTERNAL_ERROR'];

/**
 * Add `op` to the unsent list so the list ends in the same state the kiosk
 * would reach applying every op in order (app/tube_ops.cpp, tube_setting.h):
 * the newest value per row, mask and engine wins; a look clears every moved
 * row and the mask, so it drops earlier row/mask/reset/look ops; a reset
 * clears them without changing the look, so it drops earlier row/mask/reset
 * ops; one degauss is enough.
 */
function tubeAddOp(unsent, op) {
    const drops = {
        row: o => o.op === 'row' && o.key === op.key,
        mask: o => o.op === 'mask',
        engine: o => o.op === 'engine',
        look: o => ['row', 'mask', 'reset', 'look'].includes(o.op),
        reset: o => ['row', 'mask', 'reset'].includes(o.op),
    }[op.op];
    if (op.op === 'degauss' && unsent.some(o => o.op === 'degauss')) {
        return unsent;
    }
    return (drops ? unsent.filter(o => !drops(o)) : unsent).concat([op]);
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

/** Plain text for a failed POST: the server's message, or what went wrong. */
function tubeErrorText(err) {
    if (err && err.code && err.message) return err.message;
    if (err && err.name === 'SyntaxError') return 'the box did not answer properly';
    if (err && (err.name === 'AbortError' || err.name === 'TimeoutError')) return 'the box did not answer in time';
    if (err && err.name === 'TypeError') return 'the box could not be reached';
    return (err && err.message) || 'unknown error';
}

/**
 * One POST in flight at a time, at most TUBE_MAX_OPS_PER_POST ops in it (the
 * rest go next). `post(ops, signal)` resolves to {seq, accepted_at} or
 * rejects with an error carrying `code` (apiRequest's errors); `signal`
 * (an AbortSignal where the runtime has one) is aborted when the POST has had
 * no answer for `opts.postTimeoutMs` (TUBE_POST_TIMEOUT_MS). Ops added while
 * a POST is out are coalesced and sent when it returns.
 *
 * - VALIDATION_ERROR: the batch is dropped and `onRefused(message)` told.
 * - Any other failure, an abandoned POST included, puts the batch back in
 *   front of what arrived meanwhile and counts one consecutive failure
 *   (`status().failures`, `lastError` as plain text). A transient one (no
 *   code, TUBE_TRANSIENT_CODES) is retried by the next pump() — onStatus()
 *   pumps on every poll. Any other code cannot clear by itself: automatic
 *   retries stop (`status().stopped`) until the user's next change (add) or
 *   retry().
 * - onStatus(handledSeq, statusTs): a batch whose seq the kiosk has handled
 *   is confirmed. A batch still unhandled in a status written
 *   TUBE_CONFIRM_EVIDENCE_S or more after its accepted_at is lost: dropped
 *   from pending() (the controls show the kiosk's values again) and its seq
 *   remembered (`status().lost` counts them) until a later status handles it.
 *
 * pending() is what the page draws on top of the kiosk's state, so a value
 * never snaps back while its POST is out.
 */
function makeOpSender(post, onRefused, opts) {
    const postTimeoutMs = (opts && opts.postTimeoutMs) || TUBE_POST_TIMEOUT_MS;
    let unsent = [];
    let sent = [];        // [{ops, seq, acceptedAt}]
    let lostSeqs = [];    // sent batches the kiosk's own status says it has not handled
    let inFlight = null;  // the ops of the POST that has not answered yet
    let lastHandled = -1;
    let failures = 0;
    let lastError = '';
    let stopped = false;

    function failed(ops, err) {
        const code = err && err.code;
        if (code === 'VALIDATION_ERROR') {
            if (onRefused) onRefused(tubeErrorText(err));
            return;
        }
        failures += 1;
        lastError = tubeErrorText(err);
        if (code && !TUBE_TRANSIENT_CODES.includes(code)) stopped = true;
        const meanwhile = unsent;
        unsent = ops;
        for (const op of meanwhile) unsent = tubeAddOp(unsent, op);
    }

    function pump() {
        if (inFlight || unsent.length === 0) return;
        const ops = unsent.slice(0, TUBE_MAX_OPS_PER_POST);
        unsent = unsent.slice(TUBE_MAX_OPS_PER_POST);
        inFlight = ops;
        const ctl = typeof AbortController === 'function' ? new AbortController() : null;
        let settled = false;
        const settle = handler => value => {
            if (settled) return;  // the other side (answer or timeout) came first
            settled = true;
            clearTimeout(timer);
            inFlight = null;
            handler(value);
        };
        const onAnswer = settle(result => {
            failures = 0;
            lastError = '';
            const at = result && typeof result.accepted_at === 'number' ? result.accepted_at : null;
            sent = sent.concat([{ ops, seq: result.seq, acceptedAt: at }]);
            pump();
        });
        const onError = settle(err => failed(ops, err));
        const timer = setTimeout(() => {
            if (ctl) ctl.abort();
            const err = new Error('no answer');
            err.name = 'AbortError';
            onError(err);
        }, postTimeoutMs);
        Promise.resolve()
            .then(() => post(ops, ctl ? ctl.signal : undefined))
            .then(onAnswer, onError);
    }

    return {
        add(op) {
            unsent = tubeAddOp(unsent, op);
            stopped = false;  // the user acted: try again
        },
        pump,
        /** The user pressed "Try again". */
        retry() {
            stopped = false;
            pump();
        },
        onStatus(handledSeq, statusTs) {
            if (handledSeq < lastHandled) lostSeqs = [];  // a fresh kiosk: nothing to say
            sent = tubeConfirm(sent, handledSeq, lastHandled);
            lostSeqs = lostSeqs.filter(seq => seq > handledSeq);  // a late confirmation clears it
            if (typeof statusTs === 'number') {
                sent = sent.filter(b => {
                    if (b.acceptedAt === null || statusTs - b.acceptedAt < TUBE_CONFIRM_EVIDENCE_S) return true;
                    lostSeqs = lostSeqs.concat([b.seq]);
                    return false;
                });
            }
            lastHandled = handledSeq;
            if (!stopped) pump();
        },
        status() {
            return { failures, lastError, stopped, lost: lostSeqs.length };
        },
        unsent() { return unsent.slice(); },
        sent() { return sent.map(b => ({ ops: b.ops, seq: b.seq })); },
        /** Every op the kiosk has not confirmed, oldest first: sent, in flight, unsent. */
        pending() {
            const ops = sent.reduce((all, b) => all.concat(b.ops), []);
            return ops.concat(inFlight || [], unsent);
        },
    };
}

/**
 * The status line: {text, retry}. `text` is plain text (the page sets it as
 * textContent); `retry` asks for the "Try again" button.
 */
function tubeNotice(available, answered, status) {
    if (!available) return { text: answered ? 'Waiting for the TV…' : 'Loading…', retry: false };
    if (status.stopped || status.failures >= TUBE_FAILURES_BEFORE_NOTICE) {
        return { text: `Not reaching the TV: ${status.lastError}`, retry: status.stopped };
    }
    if (status.lost > 0) {
        return { text: 'A change did not reach the TV; the controls show what the TV has.', retry: false };
    }
    return { text: '', retry: false };
}

/** "30 fps" for a row that forces the 30 fps class when moved off neutral, else "". */
function tubeRateTag(row) {
    return row && row.forces_30fps ? '30 fps' : '';
}

/** Whether a row's rate tag shows: it forces 30 fps AND the box does not ignore the
    row (a real tube draws those itself in CRT Native, so they never force a rate there). */
function tubeRateTagShown(row, native) {
    return tubeRateTag(row) !== '' && !(native && !!row.crt_native_neutral);
}

/** A look's picker text: its word, plus its Modern TV rate. In CRT Native the rate
    may not apply, so it is left out (the live header rate stays). */
function tubeLookOptionText(look, native) {
    return !native && Number.isInteger(look.fps) ? `${look.word} \u00b7 ${look.fps} fps` : look.word;
}

/**
 * The frame-rate line in the header: "<n> fps", then the warning when the look is
 * too heavy for smooth video, or the unavailable notice (nothing else is true then).
 * Plain text (the page sets it as textContent); '' when the kiosk reports no rate.
 */
function tubeRateText(kiosk) {
    if (!kiosk) return '';
    if (kiosk.unavailable === 'build_hung') {
        return 'Tube could not start on this box, so it is showing the Enhanced look. Restart the box to try again.';
    }
    if (kiosk.unavailable) {
        return 'Tube could not draw this look, so the box is showing the Enhanced look. Try a different look.';
    }
    if (!Number.isInteger(kiosk.frame_rate)) return '';
    const rate = `${kiosk.frame_rate} fps`;
    return kiosk.too_heavy ? `${rate}. This look is too heavy for smooth video on this TV` : rate;
}

// ===== DOM (thin) =====

const CRT_LOOK_POLL_MS = 1000;
/** While the kiosk's status is stale (boot, a kiosk restart, right after an
    update) the tab's visibility is asked again this often, until the answer
    is anything else or the device changes. Any other "not offered" answer
    (not_ready, needs_pi5, kiosk_too_old, rows_unavailable) and a box that
    does not answer at all are never asked again on their own. */
const CRT_LOOK_RECHECK_MS = 30000;

let _crtLook = null;  // per device: see crtLookState()

function crtLookState() {
    const url = currentDevice ? currentDevice.url : null;
    if (!_crtLook || _crtLook.deviceUrl !== url) {
        if (_crtLook && _crtLook.timer) clearTimeout(_crtLook.timer);
        if (_crtLook && _crtLook.recheckTimer) clearTimeout(_crtLook.recheckTimer);
        const st = {
            deviceUrl: url, rows: null, kiosk: null, available: false, answered: false,
            running: false, timer: null, recheckTimer: null, held: null, els: null,
        };
        st.sender = makeOpSender(
            // apiRequest = apiPost plus the sender's abort signal (the wait is bounded).
            (ops, signal) => apiRequest(`${url}/admin/display/tube`, {
                method: 'POST', headers: getCsrfHeaders(), body: JSON.stringify({ ops }), signal,
            }),
            msg => showNotification(msg, 'error'));
        _crtLook = st;
        // Never leave the previous device's controls on screen.
        const body = document.getElementById('crtLookBody');
        if (body) body.innerHTML = '';
        renderCrtLook();
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

/** Show the tab only where the kiosk offers Tube. Called on every device
    change, and every CRT_LOOK_RECHECK_MS while the answer is kiosk_not_running. */
async function checkCrtLookVisibility() {
    if (!currentDevice) return false;
    const st = crtLookState();
    if (st.recheckTimer) clearTimeout(st.recheckTimer);
    st.recheckTimer = null;
    let data = null;
    try {
        data = await apiGet(crtLookTabUrl(true));
    } catch (e) {
        data = null;
    }
    if (_crtLook !== st) return false;  // the device changed while we asked
    if (data && data.available !== true && data.reason === 'kiosk_not_running') {
        st.recheckTimer = setTimeout(() => {
            st.recheckTimer = null;
            if (_crtLook === st) checkCrtLookVisibility();
        }, CRT_LOOK_RECHECK_MS);
    }
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
    if (!currentDevice) {
        crtLookState();  // shows "No box selected." with no controls
        return;
    }
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
    if (!currentDevice) {  // no box selected: drop the old box's controls, stop polling
        crtLookState();
        return;
    }
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
            st.sender.onStatus(handled, typeof data.status_ts === 'number' ? data.status_ts : null);
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

/** The status line's "Try again" button (shown after an error that will not clear by itself). */
function crtLookRetry() {
    const st = _crtLook;
    if (!st || !st.available) return;
    st.sender.retry();
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
        `<option value="${escapeHtml(l.key)}">${escapeHtml(tubeLookOptionText(l, false))}</option>`).join('');
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
                        <strong>${escapeHtml(row.label)}${tubeRateTag(row) ? ` <span class="settings-description" data-tube-rate="${escapeHtml(key)}" style="font-weight: normal; font-size: 0.8rem;">${escapeHtml(tubeRateTag(row))}</span>` : ''}</strong>
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
                <p ${desc} id="crtLookRate" role="status"></p>

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
        rate: document.getElementById('crtLookRate'),
        look: document.getElementById('crtLookLook'),
        lookHint: document.getElementById('crtLookLookHint'),
        reset: document.getElementById('crtLookReset'),
        degauss: document.getElementById('crtLookDegauss'),
        mask: body.querySelector('[data-tube-mask]'),
        ranges: {}, nums: {}, moved: {}, hints: {}, na: {}, rates: {},
    };
    body.querySelectorAll('[data-tube-row]').forEach(el => { els.ranges[el.dataset.tubeRow] = el; });
    body.querySelectorAll('[data-tube-num]').forEach(el => { els.nums[el.dataset.tubeNum] = el; });
    body.querySelectorAll('[data-tube-moved]').forEach(el => { els.moved[el.dataset.tubeMoved] = el; });
    body.querySelectorAll('[data-tube-hint]').forEach(el => { els.hints[el.dataset.tubeHint] = el; });
    body.querySelectorAll('[data-tube-rate]').forEach(el => { els.rates[el.dataset.tubeRate] = el; });
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
    const notice = st.deviceUrl === null
        ? { text: 'No box selected.', retry: false }
        : tubeNotice(st.available, st.answered, st.sender.status());
    status.textContent = notice.text;
    const box = document.getElementById('crtLookStatusBox');
    if (box) box.style.display = notice.text ? '' : 'none';
    const retry = document.getElementById('crtLookRetry');
    if (retry) retry.style.display = notice.retry ? '' : 'none';
    if (!st.rows || !st.kiosk) return;
    if (!st.els) buildCrtLook(st);
    const els = st.els;
    const rows = st.rows;
    const kiosk = st.kiosk;
    const waiting = !st.available;
    // A disabled control commonly gets no pointerup/change, so a slider held
    // when the box stopped answering would stay "held" (never repainted).
    if (waiting) st.held = null;
    const shown = tubeShown(kiosk, st.sender.pending(), rows);
    const moved = tubeMovedKeys(shown, rows);
    const native = kiosk.crt_native === true;
    const rowByKey = {};
    rows.rows.forEach(r => { rowByKey[r.key] = r; });

    els.engine.value = shown.engine;
    els.engine.disabled = waiting;
    els.engineNote.style.display = shown.engine === 'tube' ? 'none' : '';
    const rateText = tubeRateText(kiosk);
    if (els.rate.textContent !== rateText) els.rate.textContent = rateText;  // role=status: no re-announce
    Array.from(els.look.options).forEach(opt => {
        const l = rows.looks.find(x => x.key === opt.value);
        if (l) opt.textContent = tubeLookOptionText(l, native);
    });
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
        if (na && st.held === range) st.held = null;
        els.na[key].style.display = na ? '' : 'none';
        if (els.rates[key]) els.rates[key].style.display = tubeRateTagShown(rowByKey[key], native) ? '' : 'none';
        els.moved[key].style.visibility = moved.includes(key) ? 'visible' : 'hidden';
        if (st.held === range) return;  // never repaint a slider the pointer is holding
        const value = shown.values[key];
        if (typeof value === 'number') {
            range.value = String(value);
            els.nums[key].textContent = String(value);
        }
    });
}
