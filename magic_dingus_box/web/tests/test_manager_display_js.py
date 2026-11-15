"""The Content Manager's CRT Look tab (static/manager_display.js).

The page's logic — the op coalescer, what each control shows, confirmation
against the kiosk's handled_seq and the one-POST-in-flight sender — is plain
functions with no DOM access. These tests run the real file under node in a
bare `vm` context holding only setTimeout/clearTimeout/console, which also
proves the file touches nothing (no DOM, no fetch, no call) while it loads.
Skipped where node is absent, like test_escape_js.py.
"""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

STATIC = Path(__file__).resolve().parents[1] / "static"
INDEX = STATIC / "index.html"
DISPLAY_JS = STATIC / "manager_display.js"
ROWS_JSON = STATIC / "tube_rows.json"
ROWS = json.loads(ROWS_JSON.read_text(encoding="utf-8"))


def run_js(body: str):
    """Run `body` as an async function inside the file's context; return its value."""
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    script = f"""
const vm = require('vm'), fs = require('fs');
const ctx = {{ setTimeout, clearTimeout, console }};
vm.runInNewContext(fs.readFileSync({json.dumps(str(DISPLAY_JS))}, 'utf8'), ctx);
ctx.ROWS = JSON.parse(fs.readFileSync({json.dumps(str(ROWS_JSON))}, 'utf8'));
ctx.settle = () => new Promise(r => setTimeout(r, 20));
Promise.resolve(vm.runInContext({json.dumps("(async () => {" + body + "})()")}, ctx))
  .then(r => process.stdout.write(JSON.stringify(r === undefined ? null : r)))
  .catch(e => {{ process.stderr.write(String(e && e.stack || e)); process.exit(1); }});
"""
    out = subprocess.run([node, "-e", script], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    assert out.stderr == ""
    return json.loads(out.stdout)


def hum(value):
    return {"op": "row", "key": "hum", "value": value}


def test_index_loads_manager_display_last_with_a_version():
    html = INDEX.read_text()
    scripts = re.findall(r'<script src="/static/([\w.]+)\?v=\d+"></script>', html)
    assert scripts[-1] == "manager_display.js" and scripts[-2] == "vpn_settings.js"


def test_tab_buttons_are_hidden_by_default():
    html = INDEX.read_text()
    for el_id in ("crtLookTabNavBtn", "crtLookBottomNavBtn"):
        assert re.search(rf'id="{el_id}"[^>]*style="display: none;"', html)
    assert re.search(r'<section id="crtlook" class="tab-content" style="display: none;">', html)


def test_newest_value_per_row_wins_in_the_unsent_list():
    result = run_js("""
        let u = [];
        u = tubeAddOp(u, {op: 'row', key: 'hum', value: 10});
        u = tubeAddOp(u, {op: 'row', key: 'noise', value: 5});
        u = tubeAddOp(u, {op: 'row', key: 'hum', value: 20});
        let d = [];
        d = tubeAddOp(d, {op: 'degauss'});
        d = tubeAddOp(d, {op: 'degauss'});
        return {u, d};
    """)
    assert result["u"] == [{"op": "row", "key": "noise", "value": 5}, hum(20)]
    assert result["d"] == [{"op": "degauss"}]


def test_shown_is_the_kiosk_state_with_ops_on_top():
    result = run_js("""
        const values = {};
        ROWS.rows.forEach(r => { values[r.key] = 50; });
        values.hum = 20;
        const kiosk = {engine: 'tube', look: 'tv', mask: 'dots', values};
        const a = tubeShown(kiosk, [{op: 'row', key: 'noise', value: 30}], ROWS);
        const b = tubeShown(kiosk, [{op: 'row', key: 'noise', value: 30},
                                    {op: 'look', look: 'pvm'}], ROWS);
        const c = tubeShown(kiosk, [{op: 'mask', mask: 'slot'}, {op: 'engine', engine: 'classic'},
                                    {op: 'row', key: 'hum', value: 77}, {op: 'reset'}], ROWS);
        return {a, b, c, kioskHum: kiosk.values.hum};
    """)
    a, b, c = result["a"], result["b"], result["c"]
    assert a["values"]["noise"] == 30 and a["values"]["hum"] == 20
    assert a["look"] == "tv" and a["mask"] == "dots" and a["engine"] == "tube"
    pvm = next(look for look in ROWS["looks"] if look["key"] == "pvm")
    assert b["look"] == "pvm" and b["mask"] == pvm["mask"] and b["values"] == pvm["values"]
    tv = next(look for look in ROWS["looks"] if look["key"] == "tv")
    assert c["engine"] == "classic" and c["mask"] == tv["mask"] and c["values"] == tv["values"]
    assert result["kioskHum"] == 20  # the kiosk's state is not mutated


def test_moved_rows_are_the_ones_off_the_look():
    result = run_js("""
        const pvm = ROWS.looks.find(l => l.key === 'pvm');
        const shown = {look: 'pvm', mask: 'dots', values: Object.assign({}, pvm.values, {hum: 33})};
        return tubeMovedKeys(shown, ROWS);
    """)
    assert sorted(result) == ["hum", "mask"]


def test_confirmation_drops_what_the_kiosk_has_handled():
    result = run_js("""
        const sent = [{ops: [], seq: 7}, {ops: [], seq: 9}];
        return [tubeConfirm(sent, 8, 7), tubeConfirm(sent, 3, 9)];
    """)
    assert result == [[{"ops": [], "seq": 9}], []]


def test_a_handled_seq_that_goes_backwards_resets_the_page():
    result = run_js("""
        let next = 5;
        const bodies = [];
        const s = makeOpSender(ops => { bodies.push(ops); return Promise.resolve({seq: next++}); });
        s.add({op: 'row', key: 'hum', value: 10}); s.pump(); await settle();
        const first = s.sent().map(b => b.seq);
        s.onStatus(5);
        const afterConfirm = s.sent();
        s.add({op: 'row', key: 'hum', value: 11}); s.pump(); await settle();
        const second = s.sent().map(b => b.seq);
        s.onStatus(2); await settle();
        return {first, afterConfirm, second, after: s.sent(), unsent: s.unsent(), posts: bodies.length};
    """)
    assert result["first"] == [5]
    assert result["afterConfirm"] == []
    assert result["second"] == [6]
    assert result["after"] == [] and result["unsent"] == []
    assert result["posts"] == 2  # nothing re-sent


def test_sender_keeps_one_post_in_flight_and_the_newest_value_per_row_wins():
    result = run_js("""
        let seq = 0;
        const bodies = [];
        const s = makeOpSender(ops => {
            bodies.push(ops);
            return new Promise(r => setTimeout(() => r({seq: ++seq}), 1));
        });
        s.add({op: 'row', key: 'hum', value: 10}); s.pump();
        s.add({op: 'row', key: 'hum', value: 15}); s.pump();
        s.add({op: 'row', key: 'hum', value: 20}); s.pump();
        await settle();
        return {bodies, sent: s.sent(), unsent: s.unsent()};
    """)
    assert result["bodies"] == [[hum(10)], [hum(20)]]
    assert result["sent"] == [{"ops": [hum(10)], "seq": 1}, {"ops": [hum(20)], "seq": 2}]
    assert result["unsent"] == []


def test_pending_covers_the_post_in_flight_so_the_page_never_snaps_back():
    # What the controls show is the kiosk's state with pending() on top; an op
    # whose POST has not answered yet must not vanish from it.
    result = run_js("""
        let answer;
        const s = makeOpSender(ops => new Promise(r => { answer = r; }));
        s.add({op: 'look', look: 'tv'}); s.pump();
        await settle();
        s.add({op: 'row', key: 'hum', value: 9});
        const during = s.pending();
        answer({seq: 4});
        await settle();
        return {during, after: s.pending(), sent: s.sent()};
    """)
    look, hum9 = {"op": "look", "look": "tv"}, hum(9)
    assert result["during"] == [look, hum9]
    assert result["after"] == [look, hum9]
    assert result["sent"][0] == {"ops": [look], "seq": 4}


def test_failed_post_is_retried_on_the_next_pump_with_degauss_carried():
    result = run_js("""
        let calls = 0;
        const bodies = [];
        const s = makeOpSender(ops => {
            bodies.push(ops);
            calls += 1;
            if (calls === 1) {
                const e = new Error('offline'); e.code = 'NETWORK';
                return Promise.reject(e);
            }
            return Promise.resolve({seq: 40});
        });
        s.add({op: 'degauss'}); s.add({op: 'row', key: 'hum', value: 10}); s.pump();
        await settle();
        const held = s.unsent();
        s.add({op: 'row', key: 'hum', value: 12}); s.pump();
        await settle();
        return {bodies, held, sent: s.sent(), unsent: s.unsent()};
    """)
    assert result["held"] == [{"op": "degauss"}, hum(10)]
    assert result["bodies"] == [[{"op": "degauss"}, hum(10)], [{"op": "degauss"}, hum(12)]]
    assert result["sent"] == [{"ops": [{"op": "degauss"}, hum(12)], "seq": 40}]
    assert result["unsent"] == []


def test_ops_added_during_a_failing_post_stay_behind_the_failed_ones():
    result = run_js("""
        const bodies = [];
        let fail = true;
        const s = makeOpSender(ops => {
            bodies.push(ops);
            return new Promise((res, rej) => setTimeout(() => {
                if (fail) { const e = new Error('busy'); e.code = 'TUBE_BACKLOG'; rej(e); }
                else res({seq: 3});
            }, 1));
        });
        s.add({op: 'degauss'}); s.add({op: 'row', key: 'hum', value: 10}); s.pump();
        s.add({op: 'row', key: 'noise', value: 4}); s.add({op: 'row', key: 'hum', value: 11});
        s.add({op: 'degauss'});
        await settle();
        const held = s.unsent();
        fail = false;
        s.onStatus(0);   // the 1 s poll retries what is unsent
        await settle();
        return {held, posts: bodies.length, last: bodies[bodies.length - 1]};
    """)
    expected = [{"op": "degauss"}, {"op": "row", "key": "noise", "value": 4}, hum(11)]
    assert result["held"] == expected
    assert result["posts"] == 2 and result["last"] == expected


def test_a_refused_op_is_dropped_and_the_message_shown():
    result = run_js("""
        const shown = [];
        const s = makeOpSender(ops => {
            const e = new Error('Invalid Tube operation'); e.code = 'VALIDATION_ERROR';
            return Promise.reject(e);
        }, msg => shown.push(msg));
        s.add({op: 'row', key: 'hum', value: 10}); s.pump();
        await settle();
        return {shown, unsent: s.unsent(), sent: s.sent()};
    """)
    assert result == {"shown": ["Invalid Tube operation"], "unsent": [], "sent": []}
